"""
Ambilight Screen Capture Application - LOW CPU VERSION

Captures screen edge colors and sends them to Arduino via Serial.

LED Layout: 73 LEDs total
- Left side:  19 LEDs (bottom to top)
- Top side:   35 LEDs (left to right)  
- Right side: 19 LEDs (top to bottom)

Usage:
    python ambilight.py [OPTIONS]
"""

import sys
import time
import argparse
import numpy as np
import mss
import serial
import serial.tools.list_ports
import os

# 💡 LED Configuration - must match Arduino
NUM_LEDS_LEFT = 19
NUM_LEDS_TOP = 35
NUM_LEDS_RIGHT = 19
NUM_LEDS_TOTAL = NUM_LEDS_LEFT + NUM_LEDS_TOP + NUM_LEDS_RIGHT

# 📷 Screen capture zone depth (pixels from edge)
CAPTURE_DEPTH = 30  # Smaller = faster

# 📡 Serial configuration - MUST MATCH ARDUINO
BAUD_RATE = 115200


def set_low_priority():
    """⚙️ Set process to low priority to reduce system impact."""
    try:
        if sys.platform == 'win32':
            import psutil
            p = psutil.Process(os.getpid())
            p.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
            print("⚙️ Process priority set to BELOW_NORMAL")
        else:
            os.nice(10)
            print("⚙️ Process niceness set to 10")
    except:
        pass  # Ignore if we can't set priority


class Ambilight:
    """✨ Low CPU Ambilight controller."""
    def __init__(self, port=None, monitor=0, fps=60, brightness=255, 
                 saturation=1.2, smoothing=0.6):
        self.monitor_num = monitor
        self.target_fps = min(fps, 60)  # Cap at 60
        self.brightness = brightness / 255.0
        self.saturation = saturation
        self.smoothing = smoothing
        self.running = False
        
        # Screen capture - single instance
        self.sct = mss.mss()
        self._setup_monitor()
        
        # Serial connection
        self.serial = self.connect_serial(port)
        
        # Pre-allocate all buffers
        self.prev_colors = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.float32)
        self.colors = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.float32)
        self.output = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8)
        self.first_frame = True
        
        # Pre-build header (constant)
        count = NUM_LEDS_TOTAL - 1
        hi, lo = (count >> 8) & 0xFF, count & 0xFF
        self.header = bytes([ord('A'), ord('d'), ord('a'), hi, lo, hi ^ lo ^ 0x55])
        
        # FPS tracking
        self.frame_count = 0
        self.last_print = time.time()
    
    def _setup_monitor(self):
        """📺 Setup monitor and capture regions."""
        monitors = self.sct.monitors
        idx = min(self.monitor_num + 1, len(monitors) - 1)
        mon = monitors[idx]
        
        self.width = mon['width']
        self.height = mon['height']
        d = min(CAPTURE_DEPTH, 50)
        
        # Define 3 small capture regions instead of full screen
        self.left_region = {
            'left': mon['left'],
            'top': mon['top'],
            'width': d,
            'height': mon['height']
        }
        self.top_region = {
            'left': mon['left'],
            'top': mon['top'],
            'width': mon['width'],
            'height': d
        }
        self.right_region = {
            'left': mon['left'] + mon['width'] - d,
            'top': mon['top'],
            'width': d,
            'height': mon['height']
        }
        
        # Pre-compute segment sizes
        self.v_seg = self.height // NUM_LEDS_LEFT  # Vertical segment height
        self.h_seg = self.width // NUM_LEDS_TOP    # Horizontal segment width
        
        print(f"📺 Monitor {self.monitor_num}: {self.width}x{self.height}")
    
    def connect_serial(self, port=None):
        """🤖 Connect to Arduino."""
        if port is None:
            ports = list(serial.tools.list_ports.comports())
            for p in ports:
                if any(x in p.description for x in ['Arduino', 'CH340', 'USB-SERIAL', 'USB Serial']):
                    port = p.device
                    break
            if port is None and ports:
                port = ports[0].device
            if port is None:
                print("❌ No serial ports found!")
                sys.exit(1)
        
        print(f"🔌 Connecting to {port}...")
        try:
            ser = serial.Serial(port, BAUD_RATE, timeout=1)
            time.sleep(2)
            ser.reset_input_buffer()
            # Wait for ready
            for _ in range(50):
                if ser.in_waiting:
                    if 'Ada' in ser.readline().decode('utf-8', errors='ignore'):
                        print("✓ Arduino ready!")
                        return ser
                time.sleep(0.1)
            print("✓ Connected (no handshake)")
            return ser
        except serial.SerialException as e:
            print(f"✗ Connection failed: {e}")
            sys.exit(1)
    
    def capture_and_sample(self):
        """📷 Capture edges and sample colors in one pass - OPTIMIZED."""
        colors = self.colors
        
        # Capture and process LEFT edge
        shot = self.sct.grab(self.left_region)
        img = np.frombuffer(shot.raw, dtype=np.uint8).reshape(shot.height, shot.width, 4)
        seg_h = self.v_seg
        # Bottom to top (reversed)
        for i in range(NUM_LEDS_LEFT):
            y = self.height - (i + 1) * seg_h
            colors[i] = img[y:y+seg_h, :, [2,1,0]].mean(axis=(0,1))
        
        # Capture and process TOP edge  
        shot = self.sct.grab(self.top_region)
        img = np.frombuffer(shot.raw, dtype=np.uint8).reshape(shot.height, shot.width, 4)
        seg_w = self.h_seg
        idx = NUM_LEDS_LEFT
        for i in range(NUM_LEDS_TOP):
            x = i * seg_w
            colors[idx + i] = img[:, x:x+seg_w, [2,1,0]].mean(axis=(0,1))
        
        # Capture and process RIGHT edge
        shot = self.sct.grab(self.right_region)
        img = np.frombuffer(shot.raw, dtype=np.uint8).reshape(shot.height, shot.width, 4)
        idx = NUM_LEDS_LEFT + NUM_LEDS_TOP
        for i in range(NUM_LEDS_RIGHT):
            y = i * seg_h
            colors[idx + i] = img[y:y+seg_h, :, [2,1,0]].mean(axis=(0,1))
        
        return colors
    
    def process_colors(self, colors):
        """🎨 Apply brightness, saturation, and smoothing."""
        # Saturation boost
        if self.saturation != 1.0:
            gray = colors.mean(axis=1, keepdims=True)
            colors = gray + (colors - gray) * self.saturation
        
        # Brightness
        if self.brightness != 1.0:
            colors *= self.brightness
        
        # Smoothing (temporal filter)
        if self.first_frame:
            self.prev_colors[:] = colors
            self.first_frame = False
        else:
            alpha = self.smoothing
            colors = self.prev_colors * (1 - alpha) + colors * alpha
            self.prev_colors[:] = colors
        
        # Clip and convert
        np.clip(colors, 0, 255, out=colors)
        self.output[:] = colors.astype(np.uint8)
        return self.output
    
    def send(self, colors):
        """📤 Send to Arduino."""
        try:
            self.serial.write(self.header + colors.tobytes())
        except:
            self.running = False
    
    def run(self):
        """▶️  Main loop - CPU friendly."""
        print(f"\n🎨 === Ambilight STANDARD ===")
        print(f"Target Frame Rate: {self.target_fps} FPS")
        print(f"Total LEDs: {NUM_LEDS_TOTAL}")
        print(f"Smoothing: {self.smoothing}")
        print("Press Ctrl+C to stop\n")
        
        self.running = True
        frame_time = 1.0 / self.target_fps
        
        try:
            while self.running:
                t0 = time.perf_counter()
                
                # Capture, process, send
                colors = self.capture_and_sample()
                colors = self.process_colors(colors)
                self.send(colors)
                
                # Frame timing
                elapsed = time.perf_counter() - t0
                self.frame_count += 1
                
                # Print FPS every 3 seconds
                now = time.time()
                if now - self.last_print > 3.0:
                    fps = self.frame_count / (now - self.last_print)
                    print(f"📊 FPS: {fps:.1f} | Frame: {elapsed*1000:.1f}ms   ", end='\r')
                    self.frame_count = 0
                    self.last_print = now
                
                # Sleep to target FPS (yield CPU time)
                sleep_time = frame_time - elapsed - 0.001  # 1ms margin
                if sleep_time > 0:
                    time.sleep(sleep_time)
                    
        except KeyboardInterrupt:
            print("\n\n⏹️  Stopping...")
        finally:
            self.cleanup()
    
    def cleanup(self):
        """🧹 Cleanup."""
        self.running = False
        if self.serial and self.serial.is_open:
            self.send(np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8))
            time.sleep(0.05)
            self.serial.close()
            print("✅ Disconnected")


def main():
    parser = argparse.ArgumentParser(description='🌈 Ambilight - Low CPU')
    parser.add_argument('--port', '-p', type=str, default='COM3', help='Serial port 🔌')
    parser.add_argument('--monitor', '-m', type=int, default=0, help='Monitor number')
    parser.add_argument('--fps', '-f', type=int, default=30, help='Target FPS (default: 30, max recommended: 45)')
    parser.add_argument('--brightness', '-b', type=int, default=255, help='Brightness 0-255 ☀️')
    parser.add_argument('--saturation', '-s', type=float, default=1.2, help='Saturation boost 🎨')
    parser.add_argument('--smoothing', type=float, default=0.6, help='0.3=smooth, 0.7=responsive (default: 0.6) 🌊')
    parser.add_argument('--list-ports', action='store_true', help='List serial ports and exit 🔍')
    
    args = parser.parse_args()
    
    if args.list_ports:
        print("🔍 Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        return
    
    # Lower process priority
    set_low_priority()
    
    Ambilight(
        port=args.port,
        monitor=args.monitor,
        fps=args.fps,
        brightness=args.brightness,
        saturation=args.saturation,
        smoothing=args.smoothing
    ).run()


if __name__ == '__main__':
    main()
