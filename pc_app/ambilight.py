"""
Ambilight — captures screen edge colors and sends to Arduino via serial.

DXcam continuous capture (DirectX 11 desktop duplication).
73 LEDs: 19 left (bottom-top) + 35 top (left-right) + 19 right (top-bottom).

Usage: python ambilight.py [--port COM3] [--fps 30] [--monitor 0]
                            [--smoothing 1.0]
                            [--downscale 1] [--frame-skip-threshold 0]
                            [--frame-skip-stride 16] [--idle-fps 0]
                            [--idle-seconds 2.0] [--profile] [--list-ports]
"""

import sys, os, time, ctypes, argparse
import numpy as np
import dxcam
import serial, serial.tools.list_ports

# ── Config ──────────────────────────────────────────────────
LEDS_LEFT, LEDS_TOP, LEDS_RIGHT = 19, 35, 19
LEDS_TOTAL = LEDS_LEFT + LEDS_TOP + LEDS_RIGHT  # 73
EDGE_DEPTH = 50       # pixels from screen edge to sample
BAUD       = 115_200
DATA_SIZE  = LEDS_TOTAL * 3  # 219 bytes RGB

# Flicker control
DEFAULT_TEMPORAL_SMOOTHING = 1.0  # 1.0 disables smoothing; lower values apply EMA

# User-tunable runtime defaults
DEFAULT_PORT = 'COM3'
DEFAULT_MONITOR = 0
DEFAULT_FPS = 30
MAX_FPS = 240
DEFAULT_DOWNSCALE = 1
DEFAULT_FRAME_SKIP_THRESHOLD = 0.0
DEFAULT_FRAME_SKIP_STRIDE = 16
DEFAULT_IDLE_FPS = 0
DEFAULT_IDLE_SECONDS = 2.0

# Timing behavior
SERIAL_BOOT_WAIT = 2.0
HANDSHAKE_ATTEMPTS = 30
HANDSHAKE_POLL_SLEEP = 0.1
NO_FRAME_SLEEP = 0.001
STATUS_PRINT_INTERVAL = 2.0
SHUTDOWN_FLUSH_DELAY = 0.05

# Serial auto-detect keywords
AUTO_DETECT_KEYWORDS = ['Arduino', 'CH340', 'USB-SERIAL', 'ttyUSB', 'ttyACM']

# Adalight header (pre-computed, never changes)
_count = LEDS_TOTAL - 1
_hi, _lo = (_count >> 8) & 0xFF, _count & 0xFF
HEADER = bytes([ord('A'), ord('d'), ord('a'), _hi, _lo, _hi ^ _lo ^ 0x55])


def _init_windows():
    """Set 1ms timer resolution + below-normal priority."""
    if sys.platform != 'win32':
        return
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception:
        pass
    try:
        import psutil
        psutil.Process(os.getpid()).nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    except Exception:
        pass


def _cleanup_windows():
    if sys.platform != 'win32':
        return
    try:
        ctypes.windll.winmm.timeEndPeriod(1)
    except Exception:
        pass


def _build_sample_regions(width, height):
    """Pre-compute per-LED rectangles along edges for stable zone averaging."""
    seg_h = max(1, height // LEDS_LEFT)
    seg_w = max(1, width // LEDS_TOP)
    depth = max(1, min(EDGE_DEPTH, min(width, height)))

    regions = []

    # Left side: bottom to top
    for i in range(LEDS_LEFT):
        y0 = (LEDS_LEFT - 1 - i) * seg_h
        y1 = y0 + seg_h
        regions.append((y0, y1, 0, depth))

    # Top side: left to right
    for i in range(LEDS_TOP):
        x0 = i * seg_w
        x1 = x0 + seg_w
        regions.append((0, depth, x0, x1))

    # Right side: top to bottom
    for i in range(LEDS_RIGHT):
        y0 = i * seg_h
        y1 = y0 + seg_h
        regions.append((y0, y1, width - depth, width))

    # Clamp region bounds to valid frame area
    clamped = []
    for y0, y1, x0, x1 in regions:
        y0 = max(0, min(height - 1, y0))
        y1 = max(y0 + 1, min(height, y1))
        x0 = max(0, min(width - 1, x0))
        x1 = max(x0 + 1, min(width, x1))
        clamped.append((y0, y1, x0, x1))
    return clamped


def _sample_region_means(img, regions, out):
    """Fill out with RGB mean for each sampling region."""
    for i, (y0, y1, x0, x1) in enumerate(regions):
        patch = img[y0:y1, x0:x1, :3]
        out[i] = patch.reshape(-1, 3).mean(axis=0).astype(np.uint8)


def _connect_serial(port):
    """Connect to Arduino, attempt Adalight handshake."""
    if port is None:
        port = _auto_detect_port()
    print(f"Connecting to {port}...")
    ser = serial.Serial(port, BAUD, timeout=1, write_timeout=1,
                        dsrdtr=False, rtscts=False)
    time.sleep(SERIAL_BOOT_WAIT)
    ser.reset_input_buffer()
    for _ in range(HANDSHAKE_ATTEMPTS):
        if ser.in_waiting:
            if 'Ada' in ser.readline().decode('utf-8', errors='ignore'):
                print("Arduino ready")
                return ser
        time.sleep(HANDSHAKE_POLL_SLEEP)
    print("Connected (no handshake)")
    return ser


def _auto_detect_port():
    for p in serial.tools.list_ports.comports():
        if any(k in (p.description + p.device) for k in AUTO_DETECT_KEYWORDS):
            return p.device
    ports = list(serial.tools.list_ports.comports())
    if ports:
        return ports[0].device
    print("No serial ports found")
    sys.exit(1)


def run(port=DEFAULT_PORT, monitor=DEFAULT_MONITOR, fps=DEFAULT_FPS, smoothing=DEFAULT_TEMPORAL_SMOOTHING,
    downscale=DEFAULT_DOWNSCALE,
    frame_skip_threshold=DEFAULT_FRAME_SKIP_THRESHOLD,
    frame_skip_stride=DEFAULT_FRAME_SKIP_STRIDE,
    idle_fps=DEFAULT_IDLE_FPS,
    idle_seconds=DEFAULT_IDLE_SECONDS,
    profile=False):
    _init_windows()
    fps = min(fps, MAX_FPS)
    smoothing = max(0.0, min(1.0, float(smoothing)))
    smoothing_enabled = smoothing < 1.0
    downscale = max(1, int(downscale))
    frame_skip_threshold = max(0.0, float(frame_skip_threshold))
    frame_skip_stride = max(1, int(frame_skip_stride))
    idle_fps = max(0, int(idle_fps))
    idle_seconds = max(0.0, float(idle_seconds))
    idle_enabled = idle_fps > 0 and idle_fps < fps and idle_seconds > 0
    frame_time = 1.0 / fps
    idle_frame_time = 1.0 / idle_fps if idle_enabled else frame_time
    current_frame_time = frame_time
    idle_since = None

    # ── Screen capture setup (DXcam only) ──
    cam = dxcam.create(device_idx=0, output_idx=monitor, output_color="RGB")
    test = cam.grab()
    if test is None:
        time.sleep(0.1)
        test = cam.grab()
    h, w = test.shape[:2]
    cam.start(target_fps=fps, video_mode=True)
    capture_func = lambda: cam.get_latest_frame()
    capture_method = 'DXcam'

    sample_w = max(1, (w + downscale - 1) // downscale)
    sample_h = max(1, (h + downscale - 1) // downscale)
    regions = _build_sample_regions(sample_w, sample_h)

    skip_mode = "off" if frame_skip_threshold <= 0 else f"thr={frame_skip_threshold:g}, stride={frame_skip_stride}"
    print(
        f"Monitor {monitor}: {w}x{h} "
        f"| sample {sample_w}x{sample_h} (x{downscale}) "
        f"| {LEDS_TOTAL} LEDs | {fps} FPS target | {capture_method} "
        f"| frame-skip {skip_mode}"
    )

    # ── Serial setup ──
    ser = _connect_serial(port)
    buf = bytearray(len(HEADER) + DATA_SIZE)
    buf[:len(HEADER)] = HEADER
    data_slice = memoryview(buf)[len(HEADER):]

    # ── Main loop state ──
    prev = np.zeros((LEDS_TOTAL, 3), dtype=np.uint8)
    colors = np.empty((LEDS_TOTAL, 3), dtype=np.uint8)
    smoothed = np.empty((LEDS_TOTAL, 3), dtype=np.uint8)
    smooth_state = np.zeros((LEDS_TOTAL, 3), dtype=np.float32)
    smooth_initialized = False
    prev_probe_luma = None
    diff_buf = None
    frame_count = 0
    last_print = time.monotonic()

    # Optional profiler accumulators
    prof_count = 0
    prof_capture = 0.0
    prof_diff = 0.0
    prof_sample = 0.0
    prof_process = 0.0
    prof_serial = 0.0

    try:
        while True:
            t0 = time.perf_counter()
            diff_dt = 0.0
            sample_dt = 0.0
            process_dt = 0.0
            serial_dt = 0.0

            img = capture_func()
            if img is None:
                time.sleep(NO_FRAME_SLEEP)
                continue
            t1 = time.perf_counter()

            if downscale > 1:
                work_img = img[::downscale, ::downscale, :3]
            else:
                work_img = img[:, :, :3]

            skip_this_frame = False
            if frame_skip_threshold > 0:
                td0 = time.perf_counter()
                probe = work_img[::frame_skip_stride, ::frame_skip_stride, :3]
                # Luma probe is cheaper than full RGB diff and accurate enough for skip gating.
                probe_luma = (
                    probe[..., 0].astype(np.uint16)
                    + probe[..., 1].astype(np.uint16)
                    + probe[..., 2].astype(np.uint16)
                ) // 3
                if prev_probe_luma is not None:
                    if diff_buf is None or diff_buf.shape != probe_luma.shape:
                        diff_buf = np.empty_like(probe_luma, dtype=np.int16)
                    np.subtract(
                        probe_luma.astype(np.int16),
                        prev_probe_luma.astype(np.int16),
                        out=diff_buf,
                    )
                    diff = np.abs(diff_buf).mean()
                    if diff < frame_skip_threshold:
                        skip_this_frame = True
                prev_probe_luma = probe_luma
                diff_dt = time.perf_counter() - td0

            if not skip_this_frame:
                ts0 = time.perf_counter()
                _sample_region_means(work_img, regions, colors)
                sample_dt = time.perf_counter() - ts0

                tp0 = time.perf_counter()
                if smoothing_enabled:
                    # Exponential moving average to reduce tiny frame-to-frame jitter.
                    if not smooth_initialized:
                        np.copyto(smooth_state, colors, casting='unsafe')
                        smooth_initialized = True
                    else:
                        smooth_state *= (1.0 - smoothing)
                        smooth_state += smoothing * colors
                    np.clip(smooth_state, 0, 255, out=smooth_state)
                    np.copyto(smoothed, smooth_state, casting='unsafe')
                else:
                    np.copyto(smoothed, colors)
                process_dt = time.perf_counter() - tp0

                # Send only when frame output actually changes.
                tw0 = time.perf_counter()
                if not np.array_equal(smoothed, prev):
                    data_slice[:] = smoothed.tobytes()
                    ser.write(buf)
                    np.copyto(prev, smoothed)
                    idle_since = None
                    current_frame_time = frame_time
                elif idle_enabled:
                    if idle_since is None:
                        idle_since = time.monotonic()
                    elif (time.monotonic() - idle_since) >= idle_seconds:
                        current_frame_time = idle_frame_time
                serial_dt = time.perf_counter() - tw0

            frame_count += 1

            if profile:
                prof_count += 1
                prof_capture += (t1 - t0)
                prof_diff += diff_dt
                prof_sample += sample_dt
                prof_process += process_dt
                prof_serial += serial_dt

            # Status print every 2 seconds
            now = time.monotonic()
            if now - last_print >= STATUS_PRINT_INTERVAL:
                fps_now = frame_count / (now - last_print)
                if profile and prof_count:
                    capture_ms = (prof_capture / prof_count) * 1000
                    sampling_ms = (prof_sample / prof_count) * 1000
                    processing_ms = (prof_process / prof_count) * 1000
                    serial_ms = (prof_serial / prof_count) * 1000
                    if frame_skip_threshold > 0:
                        diff_us = (prof_diff / prof_count) * 1_000_000
                        print(
                            f"FPS: {fps_now:.1f} | capture:{capture_ms:.2f}ms "
                            f"frame_difference:{diff_us:.0f}us "
                            f"sampling:{sampling_ms:.2f}ms "
                            f"processing:{processing_ms:.2f}ms "
                            f"serial_write:{serial_ms:.2f}ms",
                            end='\r'
                        )
                    else:
                        print(
                            f"FPS: {fps_now:.1f} | capture:{capture_ms:.2f}ms "
                            f"sampling:{sampling_ms:.2f}ms "
                            f"processing:{processing_ms:.2f}ms "
                            f"serial_write:{serial_ms:.2f}ms",
                            end='\r'
                        )
                    prof_count = 0
                    prof_capture = 0.0
                    prof_diff = 0.0
                    prof_sample = 0.0
                    prof_process = 0.0
                    prof_serial = 0.0
                else:
                    print(f"FPS: {fps_now:.1f}", end='\r')
                frame_count = 0
                last_print = now

            # Sleep remainder of frame
            remaining = current_frame_time - (time.perf_counter() - t0)
            if remaining > 0:
                time.sleep(remaining)

    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        try:
            cam.stop()
        except Exception:
            pass
        try:
            cam.release()
        except Exception:
            pass
        # Blank LEDs
        data_slice[:] = b'\x00' * DATA_SIZE
        ser.write(buf)
        time.sleep(SHUTDOWN_FLUSH_DELAY)
        ser.close()
        _cleanup_windows()
        print("Shutdown complete")


def main():
    parser = argparse.ArgumentParser(description='Ambilight')
    parser.add_argument('--port', '-p', default=DEFAULT_PORT)
    parser.add_argument('--monitor', '-m', type=int, default=DEFAULT_MONITOR)
    parser.add_argument('--fps', '-f', type=int, default=DEFAULT_FPS)
    parser.add_argument('--smoothing', type=float, default=DEFAULT_TEMPORAL_SMOOTHING,
                        help='EMA smoothing [0..1]. 1 disables smoothing (default).')
    parser.add_argument('--downscale', type=int, default=DEFAULT_DOWNSCALE,
                        help='Capture downscale factor for sampling (1 disables).')
    parser.add_argument('--frame-skip-threshold', type=float,
                        default=DEFAULT_FRAME_SKIP_THRESHOLD,
                        help='Skip LED processing when frame delta is below threshold (0 disables).')
    parser.add_argument('--frame-skip-stride', type=int, default=DEFAULT_FRAME_SKIP_STRIDE,
                        help='Stride used by frame-difference probe (higher is cheaper).')
    parser.add_argument('--idle-fps', type=int, default=DEFAULT_IDLE_FPS,
                        help='Fallback FPS after scene is stable for --idle-seconds (0 disables).')
    parser.add_argument('--idle-seconds', type=float, default=DEFAULT_IDLE_SECONDS,
                        help='Seconds with unchanged LEDs before switching to --idle-fps.')
    parser.add_argument('--profile', action='store_true',
                        help='Print average timing for capture/frame_difference/sampling/processing/serial_write.')
    parser.add_argument('--list-ports', action='store_true')
    args = parser.parse_args()

    if args.list_ports:
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        return

    run(
        port=args.port,
        monitor=args.monitor,
        fps=args.fps,
        smoothing=args.smoothing,
        downscale=args.downscale,
        frame_skip_threshold=args.frame_skip_threshold,
        frame_skip_stride=args.frame_skip_stride,
        idle_fps=args.idle_fps,
        idle_seconds=args.idle_seconds,
        profile=args.profile,
    )


if __name__ == '__main__':
    main()
