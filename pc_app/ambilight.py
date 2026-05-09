"""
Ambilight Screen Capture

Captures screen edge colors and sends them to Arduino via Serial.
Single full-screen DXcam grab (DirectX 11 desktop duplication),
uint8 pipeline throughout, pre-allocated serial buffer.

LED Layout: 73 LEDs total
- Left side:  19 LEDs (bottom to top)
- Top side:   35 LEDs (left to right)
- Right side: 19 LEDs (top to bottom)

Usage: python ambilight.py [OPTIONS]
"""

import sys
import time
import argparse
import threading
import numpy as np
import dxcam
import serial
import serial.tools.list_ports
import os

# ============================================================
# ⚙️  CONFIGURATION — edit these to tune behaviour
# ============================================================

# 💡 LED layout (must match Arduino)
NUM_LEDS_LEFT  = 19
NUM_LEDS_TOP   = 35
NUM_LEDS_RIGHT = 19
NUM_LEDS_TOTAL = NUM_LEDS_LEFT + NUM_LEDS_TOP + NUM_LEDS_RIGHT

# 📺 Screen capture
CAPTURE_DEPTH = 60  # Pixels from screen edge to sample
DOWNSAMPLE    = 4   # Sample every Nth pixel (dxcam is fast, can afford denser)

# 📡 Serial
BAUD_RATE = 115200

# 🧵 Ring buffer size (frames held between capture and send threads)
BUFFER_SIZE = 2

# 🔋 Adaptive idle (static screen)
STATIC_INTERVAL  = 1.0   # Seconds between captures when screen is static
STATIC_THRESHOLD = 100   # Consecutive identical frames before entering static mode

# 🎯 Defaults (overridable via CLI)
DEFAULT_PORT    = 'COM3'
DEFAULT_MONITOR = 0
DEFAULT_FPS     = 60

# ============================================================


def set_process_priority():
    try:
        if sys.platform == 'win32':
            import psutil
            psutil.Process(os.getpid()).nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
            print("⚙️  Priority: BELOW_NORMAL")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Ring buffer — non-blocking, always returns the most recent frame
# ---------------------------------------------------------------------------

class RingBuffer:
    def __init__(self, size, shape, dtype=np.uint8):
        self.size    = size
        self.buffers = [np.zeros(shape, dtype=dtype) for _ in range(size)]
        self.write_idx = 0
        self.count   = 0
        self.lock    = threading.Lock()

    def put(self, data):
        with self.lock:
            np.copyto(self.buffers[self.write_idx], data)
            self.write_idx = (self.write_idx + 1) % self.size
            if self.count < self.size:
                self.count += 1

    def get(self):
        """Return a copy of the most recent frame, or None if empty."""
        with self.lock:
            if self.count == 0:
                return None
            idx = (self.write_idx - 1) % self.size
            return self.buffers[idx].copy()


# ---------------------------------------------------------------------------
# ScreenCapture — single DXcam grab, vectorized LED averaging
# ---------------------------------------------------------------------------

class ScreenCapture:
    def __init__(self, monitor_num=DEFAULT_MONITOR):
        self.camera = dxcam.create(device_idx=0, output_idx=monitor_num,
                                   output_color="RGB")
        # Grab one frame to get dimensions
        test = self.camera.grab()
        if test is None:
            time.sleep(0.1)
            test = self.camera.grab()
        self.height, self.width = test.shape[:2]
        d  = CAPTURE_DEPTH
        ds = DOWNSAMPLE

        print(f"📺 Monitor {monitor_num}: {self.width}x{self.height}, "
              f"depth={d}px, downsample={ds}x (DXcam)")

        self._d  = d
        self._ds = ds
        self._result = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8)

        # Pre-compute downsampled segment sizes
        self._v_seg_ds = max(1, (self.height // NUM_LEDS_LEFT) // ds)
        self._h_seg_ds = max(1, (self.width  // NUM_LEDS_TOP)  // ds)

        # Timing stats
        self.grab_ms   = 0.0
        self.sample_ms = 0.0

    def capture(self):
        """Grab full screen once, then average each LED segment."""
        t0  = time.perf_counter()
        img = self.camera.grab()
        if img is None:
            return self._result
        t1 = time.perf_counter()

        d  = self._d
        ds = self._ds
        out = self._result
        v_seg_ds = self._v_seg_ds
        h_seg_ds = self._h_seg_ds

        # Extract edge strips (numpy views, no copy)
        left_strip  = img[::ds, :d:ds, :]
        top_strip   = img[:d:ds, ::ds, :]
        right_strip = img[::ds, -d::ds, :]

        # Left: vertical segments, bottom-to-top
        h_left = left_strip.shape[0]
        for i in range(NUM_LEDS_LEFT):
            y_end   = h_left - i * v_seg_ds
            y_start = max(0, y_end - v_seg_ds)
            out[i] = left_strip[y_start:y_end].mean(axis=(0, 1))

        # Top: horizontal segments, left-to-right
        off = NUM_LEDS_LEFT
        w_top = top_strip.shape[1]
        for i in range(NUM_LEDS_TOP):
            x_start = i * h_seg_ds
            x_end   = min(w_top, x_start + h_seg_ds)
            out[off + i] = top_strip[:, x_start:x_end].mean(axis=(0, 1))

        # Right: vertical segments, top-to-bottom
        off2 = NUM_LEDS_LEFT + NUM_LEDS_TOP
        h_right = right_strip.shape[0]
        for i in range(NUM_LEDS_RIGHT):
            y_start = i * v_seg_ds
            y_end   = min(h_right, y_start + v_seg_ds)
            out[off2 + i] = right_strip[y_start:y_end].mean(axis=(0, 1))

        t2 = time.perf_counter()
        self.grab_ms   = (t1 - t0) * 1000
        self.sample_ms = (t2 - t1) * 1000
        return out

    def stop(self):
        try:
            self.camera.release()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# SerialSender — pre-allocated buffer, zero-copy writes
# ---------------------------------------------------------------------------

class SerialSender:
    HEADER_SIZE = 6

    def __init__(self, port=DEFAULT_PORT):
        self.serial = self._connect(port)
        count      = NUM_LEDS_TOTAL - 1
        hi, lo     = (count >> 8) & 0xFF, count & 0xFF
        # Pre-allocate send buffer: header + RGB data
        self._buf = bytearray(self.HEADER_SIZE + NUM_LEDS_TOTAL * 3)
        self._buf[0:6] = bytes([ord('A'), ord('d'), ord('a'),
                                hi, lo, hi ^ lo ^ 0x55])
        self._data_view = memoryview(self._buf)[self.HEADER_SIZE:]

    def _connect(self, port):
        if port is None:
            port = self._auto_detect()
        print(f"🔌 Connecting to {port}...")
        try:
            ser = serial.Serial(port, BAUD_RATE, timeout=1)
            time.sleep(2)
            ser.reset_input_buffer()
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
            try:
                for p in serial.tools.list_ports.comports():
                    print(f"  - {p.device}: {p.description}")
            except Exception:
                pass
            sys.exit(1)

    def _auto_detect(self):
        keywords = ['Arduino', 'CH340', 'USB-SERIAL', 'ttyUSB', 'ttyACM']
        ports    = list(serial.tools.list_ports.comports())
        for p in ports:
            if any(k in p.description or k in p.device for k in keywords):
                return p.device
        if ports:
            return ports[0].device
        print("❌ No serial ports found!")
        sys.exit(1)

    def send(self, colors):
        try:
            self._data_view[:] = colors.tobytes()
            self.serial.write(self._buf)
            return True
        except Exception:
            return False

    def close(self):
        if self.serial and self.serial.is_open:
            self._data_view[:] = b'\x00' * (NUM_LEDS_TOTAL * 3)
            self.serial.write(self._buf)
            time.sleep(0.05)
            self.serial.close()


# ---------------------------------------------------------------------------
# Ambilight — main controller
# ---------------------------------------------------------------------------

class Ambilight:
    def __init__(self, port=DEFAULT_PORT, monitor=DEFAULT_MONITOR, fps=DEFAULT_FPS):

        self.target_fps = min(fps, 120)
        self.running    = False

        self.capture = ScreenCapture(monitor)
        self.sender  = SerialSender(port)

        self.color_buffer   = RingBuffer(BUFFER_SIZE, (NUM_LEDS_TOTAL, 3), dtype=np.uint8)
        self.capture_thread = None

        self.prev_output    = np.zeros((NUM_LEDS_TOTAL, 3), dtype=np.uint8)
        self.static_frames  = 0
        self.skipped_frames = 0
        self.is_static      = False
        self._last_static_send = 0.0

        self.stats = {'capture': 0.0, 'send': 0.0}

    def _capture_loop(self):
        """Capture thread: single DXcam grab + LED sampling."""
        while self.running:
            t0     = time.perf_counter()
            colors = self.capture.capture()
            self.color_buffer.put(colors)
            self.stats['capture'] = time.perf_counter() - t0

            if self.is_static:
                elapsed    = time.perf_counter() - t0
                sleep_time = STATIC_INTERVAL - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

    def run(self):
        print(f"\n=== Ambilight (DXcam) ===")
        print(f"FPS target : {self.target_fps}")
        print(f"LEDs       : {NUM_LEDS_TOTAL}  ({NUM_LEDS_LEFT}L + {NUM_LEDS_TOP}T + {NUM_LEDS_RIGHT}R)")
        print(f"Downsample : {DOWNSAMPLE}x")
        print(f"Buffer     : {BUFFER_SIZE} frames")
        print("Press Ctrl+C to stop\n")

        self.running = True
        frame_time   = 1.0 / self.target_fps

        self.capture_thread = threading.Thread(
            target=self._capture_loop, daemon=True)
        self.capture_thread.start()

        time.sleep(0.15)

        frame_count = 0
        last_print  = time.time()

        try:
            while self.running:
                t0 = time.perf_counter()

                output = self.color_buffer.get()
                if output is None:
                    time.sleep(0.001)
                    continue

                if np.array_equal(output, self.prev_output):
                    self.static_frames  += 1
                    self.skipped_frames += 1
                    self.is_static = self.static_frames > STATIC_THRESHOLD
                    # Keepalive: resend once per STATIC_INTERVAL
                    if self.is_static:
                        now_pc = time.perf_counter()
                        if now_pc - self._last_static_send >= STATIC_INTERVAL:
                            t2 = time.perf_counter()
                            self.sender.send(output)
                            self.stats['send'] = time.perf_counter() - t2
                            self._last_static_send = now_pc
                else:
                    t2 = time.perf_counter()
                    if not self.sender.send(output):
                        print("\nSerial error!")
                        break
                    self.stats['send'] = time.perf_counter() - t2
                    np.copyto(self.prev_output, output)
                    self.static_frames = 0
                    self.is_static     = False

                frame_count += 1

                now = time.time()
                if now - last_print > 0.5:
                    fps      = frame_count / (now - last_print)
                    skip_pct = self.skipped_frames / max(frame_count, 1) * 100
                    mode     = 'STATIC' if self.is_static else 'ACTIVE'
                    print(
                        f"FPS:{fps:5.1f} | {mode} | Skip:{skip_pct:3.0f}% | "
                        f"Grab:{self.capture.grab_ms:4.1f}ms "
                        f"Samp:{self.capture.sample_ms:4.1f}ms "
                        f"Ser:{self.stats['send']*1000:4.1f}ms   ",
                        end='\r')
                    frame_count         = 0
                    self.skipped_frames = 0
                    last_print          = now

                elapsed    = time.perf_counter() - t0
                sleep_time = (STATIC_INTERVAL if self.is_static else frame_time) - elapsed - 0.0005
                if sleep_time > 0:
                    time.sleep(sleep_time)

        except KeyboardInterrupt:
            print("\n\n⏹️  Stopping...")
        finally:
            self.cleanup()

    def cleanup(self):
        self.running = False
        if self.capture_thread:
            self.capture_thread.join(timeout=1.0)
        self.capture.stop()
        self.sender.close()
        print("✅ Shutdown complete")


def list_ports():
    print("🔍 Available ports:")
    for p in serial.tools.list_ports.comports():
        print(f"  {p.device}: {p.description}")


def main():
    parser = argparse.ArgumentParser(
        description='Ambilight',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
Examples:
  python ambilight.py                         # auto-detect port, {DEFAULT_FPS} FPS
  python ambilight.py --fps 45               # cap at 45 FPS
  python ambilight.py --port COM3            # specific port
  python ambilight.py --list-ports           # show available ports
        """)

    parser.add_argument('--port',    '-p', type=str, default=DEFAULT_PORT,
                        help=f'Serial port (default: {DEFAULT_PORT})')
    parser.add_argument('--monitor', '-m', type=int, default=DEFAULT_MONITOR,
                        help=f'Monitor number (default: {DEFAULT_MONITOR})')
    parser.add_argument('--fps',     '-f', type=int, default=DEFAULT_FPS,
                        help=f'Target FPS (default: {DEFAULT_FPS})')
    parser.add_argument('--list-ports', action='store_true',
                        help='List serial ports and exit')

    args = parser.parse_args()

    if args.list_ports:
        list_ports()
        return

    set_process_priority()

    Ambilight(
        port=args.port,
        monitor=args.monitor,
        fps=args.fps,
    ).run()


if __name__ == '__main__':
    main()
