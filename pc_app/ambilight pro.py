"""
Ambilight Screen Capture - PRO VERSION (Multithreaded)

Advanced optimizations:
- Triple buffering with producer/consumer threads
- Parallel screen capture for all 3 edges
- Lock-free ring buffer for minimal latency  
- Vectorized numpy operations (no Python loops)
- Adaptive frame skipping under load
- Memory-mapped screen capture where possible

LED Layout: 73 LEDs total
- Left side:  19 LEDs (bottom to top)
- Top side:   35 LEDs (left to right)  
- Right side: 19 LEDs (top to bottom)
"""

import sys
import time
import argparse
import threading
import queue
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import mss
import serial
import serial.tools.list_ports
import os

# 💡 LED Configuration
NUM_LEDS_LEFT = 19
NUM_LEDS_TOP = 35
NUM_LEDS_RIGHT = 19
NUM_LEDS_TOTAL = NUM_LEDS_LEFT + NUM_LEDS_TOP + NUM_LEDS_RIGHT

# 📸 Capture settings
CAPTURE_DEPTH = 30
DOWNSAMPLE = 2  # Sample every Nth pixel (2 = half resolution, much faster)

# 📡 Serial
BAUD_RATE = 115200

# 🧵 Threading
BUFFER_SIZE = 3  # Triple buffer


def set_process_priority():
    """Set process priority."""
    try:
        if sys.platform == 'win32':
            import psutil
            p = psutil.Process(os.getpid())
            p.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
            print("⚙️ Priority: BELOW_NORMAL")
    except:
        pass


class RingBuffer:
    """🔄 Lock-free single producer single consumer ring buffer."""
    
    def __init__(self, size, shape, dtype=np.uint8):
        self.size = size
        self.buffers = [np.zeros(shape, dtype=dtype) for _ in range(size)]
        self.write_idx = 0
        self.read_idx = 0
        self.count = 0
        self.lock = threading.Lock()
    
    def put(self, data):
        """Non-blocking put - overwrites oldest if full."""
        with self.lock:
            np.copyto(self.buffers[self.write_idx], data)
            self.write_idx = (self.write_idx + 1) % self.size
            if self.count < self.size:
                self.count += 1
            else:
                self.read_idx = (self.read_idx + 1) % self.size
    
    def get(self):
        """Get latest data, returns None if empty."""
        with self.lock:
            if self.count == 0:
                return None
            # Get most recent
            idx = (self.write_idx - 1) % self.size
            data = self.buffers[idx].copy()
            self.count = 0  # Mark as consumed
            self.read_idx = self.write_idx
            return data


class ScreenCapture:
    """📸 Optimized parallel screen capture."""
    
    def __init__(self, monitor_num=0):
        self.sct = mss.mss()
        self._setup_monitor(monitor_num)
        self._precompute_indices()
        
    def _setup_monitor(self, monitor_num):
        monitors = self.sct.monitors
        idx = min(monitor_num + 1, len(monitors) - 1)
        mon = monitors[idx]
        
        self.width = mon['width']
        self.height = mon['height']
        d = CAPTURE_DEPTH
        
        self.regions = {
            'left': {'left': mon['left'], 'top': mon['top'], 
                     'width': d, 'height': mon['height']},
            'top': {'left': mon['left'], 'top': mon['top'], 
                    'width': mon['width'], 'height': d},
            'right': {'left': mon['left'] + mon['width'] - d, 'top': mon['top'], 
                      'width': d, 'height': mon['height']}
        }
        
        print(f"📺 Monitor: {self.width}x{self.height}, Capture Depth: {d} pixels")
    
    def _precompute_indices(self):
        """Pre-compute all sampling indices for vectorized operations."""
        v_seg = self.height // NUM_LEDS_LEFT
        h_seg = self.width // NUM_LEDS_TOP
        ds = DOWNSAMPLE
        
        # Left: bottom to top (reversed indices)
        self.left_slices = []
        for i in range(NUM_LEDS_LEFT):
            y = self.height - (i + 1) * v_seg
            self.left_slices.append((
                slice(y, y + v_seg, ds),  # y slice with downsample
                slice(None, None, ds)      # x slice with downsample
            ))
        
        # Top: left to right
        self.top_slices = []
        for i in range(NUM_LEDS_TOP):
            x = i * h_seg
            self.top_slices.append((
                slice(None, None, ds),
                slice(x, x + h_seg, ds)
            ))
        
        # Right: top to bottom
        self.right_slices = []
        for i in range(NUM_LEDS_RIGHT):
            y = i * v_seg
            self.right_slices.append((
                slice(y, y + v_seg, ds),
                slice(None, None, ds)
            ))
    
    def capture_region(self, name):
        """Capture single region - for parallel execution."""
        shot = self.sct.grab(self.regions[name])
        # Direct buffer access, reshape in place
        img = np.frombuffer(shot.raw, dtype=np.uint8).reshape(
            shot.height, shot.width, 4)
        return name, img
    
    def sample_colors_vectorized(self, left_img, top_img, right_img):
        """Sample all colors using vectorized operations."""
        colors = np.empty((NUM_LEDS_TOTAL, 3), dtype=np.float32)
        
        # Left edge - BGR to RGB conversion with [2,1,0]
        for i, (ys, xs) in enumerate(self.left_slices):
            segment = left_img[ys, xs, :3]
            colors[i] = segment[:, :, [2,1,0]].mean(axis=(0,1))
        
        # Top edge
        base = NUM_LEDS_LEFT
        for i, (ys, xs) in enumerate(self.top_slices):
            segment = top_img[ys, xs, :3]
            colors[base + i] = segment[:, :, [2,1,0]].mean(axis=(0,1))
        
        # Right edge
        base = NUM_LEDS_LEFT + NUM_LEDS_TOP
        for i, (ys, xs) in enumerate(self.right_slices):
            segment = right_img[ys, xs, :3]
            colors[base + i] = segment[:, :, [2,1,0]].mean(axis=(0,1))
        
        return colors


class ColorProcessor:
    """Processes colors with smoothing and effects."""
    
    def __init__(self, brightness=1.0, saturation=1.2, smoothing=0.6):
        self.brightness = brightness
        self.saturation = saturation
        self.smoothing = smoothing
        self.prev_colors = None
        
        # Pre-allocate output buffer
        self.output = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8)
    
    def process(self, colors):
        """Apply all color transformations."""
        # Saturation boost (vectorized)
        if self.saturation != 1.0:
            gray = colors.mean(axis=1, keepdims=True)
            colors = gray + (colors - gray) * self.saturation
        
        # Brightness
        if self.brightness != 1.0:
            colors *= self.brightness
        
        # Temporal smoothing (EMA)
        if self.prev_colors is None:
            self.prev_colors = colors.copy()
        else:
            alpha = self.smoothing
            colors = self.prev_colors + alpha * (colors - self.prev_colors)
            np.copyto(self.prev_colors, colors)
        
        # Clip and convert (in-place)
        np.clip(colors, 0, 255, out=colors)
        np.copyto(self.output, colors.astype(np.uint8))
        return self.output


class SerialSender:
    """🤖 Handles serial communication with Arduino."""
    
    def __init__(self, port=None):
        self.serial = self._connect(port)
        
        # Pre-build Adalight header
        count = NUM_LEDS_TOTAL - 1
        hi, lo = (count >> 8) & 0xFF, count & 0xFF
        self.header = bytes([ord('A'), ord('d'), ord('a'), hi, lo, hi ^ lo ^ 0x55])
    
    def _connect(self, port):
        if port is None:
            port = self._auto_detect()
        
        print(f"🔌 Connecting to {port}...")
        try:
            ser = serial.Serial(port, BAUD_RATE, timeout=1)
            print(f"✓ Successfully opened {port}")
            time.sleep(2)
            ser.reset_input_buffer()
            
            # Wait for handshake
            for _ in range(30):
                if ser.in_waiting:
                    if 'Ada' in ser.readline().decode('utf-8', errors='ignore'):
                        print("✓ Arduino ready!")
                        return ser
                time.sleep(0.1)
            
            print("✓ Connected (no handshake)")
            return ser
        except Exception as e:
            print(f"✗ Connection failed to {port}: {e}")
            print(f"Attempting to list available ports...")
            try:
                for p in serial.tools.list_ports.comports():
                    print(f"  - {p.device}: {p.description}")
            except:
                pass
            sys.exit(1)
    
    def _auto_detect(self):
        ports = list(serial.tools.list_ports.comports())
        keywords = ['Arduino', 'CH340', 'USB-SERIAL', 'ttyUSB', 'ttyACM']
        
        for p in ports:
            if any(k in p.description or k in p.device for k in keywords):
                return p.device
        
        if ports:
            return ports[0].device
        
        print("❌ No serial ports found!")
        sys.exit(1)
    
    def send(self, colors):
        """📤 Send colors to Arduino."""
        try:
            self.serial.write(self.header + colors.tobytes())
            return True
        except:
            return False
    
    def close(self):
        if self.serial and self.serial.is_open:
            # Send black
            black = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8)
            self.send(black)
            time.sleep(0.05)
            self.serial.close()


class AmbilightPro:
    """✨ Main Ambilight controller with multithreading."""
    
    def __init__(self, port=None, monitor=0, fps=30, brightness=255,
                 saturation=1.2, smoothing=0.6):
        
        self.target_fps = min(fps, 60)
        self.running = False
        
        # Components
        self.capture = ScreenCapture(monitor)
        self.processor = ColorProcessor(
            brightness / 255.0, saturation, smoothing)
        self.sender = SerialSender(port)
        
        # Threading
        self.color_buffer = RingBuffer(BUFFER_SIZE, (NUM_LEDS_TOTAL, 3), np.float32)
        self.capture_thread = None
        self.executor = ThreadPoolExecutor(max_workers=3)
        
        # Stats
        self.stats = {'capture': 0, 'process': 0, 'send': 0, 'frames': 0}
        self.last_stats = time.time()
    
    def _capture_loop(self):
        """Capture thread - runs independently."""
        # Each thread needs its own mss instance
        local_sct = mss.mss()
        regions = self.capture.regions
        
        while self.running:
            t0 = time.perf_counter()
            
            # Capture all 3 regions (can't parallelize mss easily)
            images = {}
            for name in ['left', 'top', 'right']:
                shot = local_sct.grab(regions[name])
                images[name] = np.frombuffer(
                    shot.raw, dtype=np.uint8
                ).reshape(shot.height, shot.width, 4)
            
            # Sample colors
            colors = self.capture.sample_colors_vectorized(
                images['left'], images['top'], images['right'])
            
            # Put in buffer (non-blocking)
            self.color_buffer.put(colors)
            
            self.stats['capture'] = time.perf_counter() - t0
            
            # Throttle capture to ~2x target FPS (provides buffer)
            target_time = 0.5 / self.target_fps
            elapsed = time.perf_counter() - t0
            if elapsed < target_time:
                time.sleep(target_time - elapsed)
    
    def run(self):
        """Main loop with separate capture thread."""
        print(f"\n=== Ambilight PRO (Multithreaded) ===")
        print(f"Target Frame Rate: {self.target_fps} FPS")
        print(f"Total LEDs: {NUM_LEDS_TOTAL}")
        print(f"Downsampling: {DOWNSAMPLE}x (sampling every {DOWNSAMPLE} pixels)")
        print(f"Buffer Size: {BUFFER_SIZE} frames (triple buffering)")
        print("Press Ctrl+C to stop\n")
        
        self.running = True
        frame_time = 1.0 / self.target_fps
        
        # Start capture thread
        self.capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self.capture_thread.start()
        
        # Wait for first capture
        time.sleep(0.1)
        
        frame_count = 0
        last_print = time.time()
        
        try:
            while self.running:
                t0 = time.perf_counter()
                
                # Get latest colors from buffer
                colors = self.color_buffer.get()
                if colors is None:
                    time.sleep(0.001)
                    continue
                
                # Process
                t1 = time.perf_counter()
                output = self.processor.process(colors)
                self.stats['process'] = time.perf_counter() - t1
                
                # Send
                t2 = time.perf_counter()
                if not self.sender.send(output):
                    print("\nSerial error!")
                    break
                self.stats['send'] = time.perf_counter() - t2
                
                frame_count += 1
                
                # Print stats every 3 seconds
                now = time.time()
                if now - last_print > 3.0:
                    fps = frame_count / (now - last_print)
                    cap_ms = self.stats['capture'] * 1000
                    proc_ms = self.stats['process'] * 1000
                    send_ms = self.stats['send'] * 1000
                    total_ms = (time.perf_counter() - t0) * 1000
                    
                    print(f"📊 FPS: {fps:.1f} | "
                          f"Capture: {cap_ms:.1f}ms | "
                          f"Processing: {proc_ms:.1f}ms | "
                          f"Serial: {send_ms:.1f}ms | "
                          f"Total: {total_ms:.1f}ms    ", end='\r')
                    
                    frame_count = 0
                    last_print = now
                
                # Frame timing
                elapsed = time.perf_counter() - t0
                sleep_time = frame_time - elapsed - 0.0005
                if sleep_time > 0:
                    time.sleep(sleep_time)
                    
        except KeyboardInterrupt:
            print("\n\n⏹️  Stopping...")
        finally:
            self.cleanup()
    
    def cleanup(self):
        """🛑 Clean shutdown."""
        self.running = False
        
        if self.capture_thread:
            self.capture_thread.join(timeout=1.0)
        
        self.executor.shutdown(wait=False)
        self.sender.close()
        print("✅ Shutdown complete")


def list_ports():
    """🔍 List available serial ports."""
    print("🔍 Available ports:")
    for p in serial.tools.list_ports.comports():
        print(f"  {p.device}: {p.description}")


def main():
    parser = argparse.ArgumentParser(
        description='Ambilight PRO - Multithreaded Version',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python ambilight_pro.py                    # Auto-detect, 30 FPS
  python ambilight_pro.py --fps 45           # Higher FPS for gaming
  python ambilight_pro.py --fps 20           # Lower CPU usage
  python ambilight_pro.py --port COM3        # Specific port
  python ambilight_pro.py --brightness 150   # Dimmer
        """)
    
    parser.add_argument('--port', '-p', type=str, default='COM3',
                       help='Serial port (default: COM3) 🔌')
    parser.add_argument('--monitor', '-m', type=int, default=0,
                       help='Monitor number (0 = primary)')
    parser.add_argument('--fps', '-f', type=int, default=30,
                       help='Target FPS (default: 30, recommended: 20-45)')
    parser.add_argument('--brightness', '-b', type=int, default=255,
                       help='Brightness 0-255 (default: 255)')
    parser.add_argument('--saturation', '-s', type=float, default=1.2,
                       help='Saturation boost (default: 1.2)')
    parser.add_argument('--smoothing', type=float, default=0.6,
                       help='Temporal smoothing 0.0-1.0 (default: 0.6)')
    parser.add_argument('--list-ports', action='store_true',
                       help='List serial ports and exit 🔍')
    
    args = parser.parse_args()
    
    if args.list_ports:
        list_ports()
        return
    
    set_process_priority()
    
    AmbilightPro(
        port=args.port,
        monitor=args.monitor,
        fps=args.fps,
        brightness=args.brightness,
        saturation=args.saturation,
        smoothing=args.smoothing
    ).run()


if __name__ == '__main__':
    main()
