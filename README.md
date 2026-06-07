# 💡 Ambilight

DIY ambient lighting that mirrors your monitor's edge colors onto SK6812 RGBW LED strips using Adalight serial frames.

| Component | Location |
|-----------|----------|
| 🔧 Arduino firmware | `src/main.cpp` |
| 🐍 Python sender | `pc_app/ambilight.py` |
| ⚡ Native C++ sender | `pc_app_native/` |

---

## 🖥️ LED Layout

```
     ┌────────────────── TOP (35) ──────────────────┐
     │              ← left to right →               │
 ┌───┤                                              ├───┐
 │ L │                                              │ R │
 │ E │                                              │ I │
 │ F │               MONITOR                        │ G │
 │ T │                                              │ H │
 │19 │                                              │T19│
 │ ↑ │                                              │ ↓ │
 └───┴──────────────────────────────────────────────┴───┘
  START                                               END
```

- **Left** — 19 LEDs, bottom → top
- **Top** — 35 LEDs, left → right
- **Right** — 19 LEDs, top → bottom
- **Total** — 73 LEDs

---

## 🔩 Hardware

| Item | Spec |
|------|------|
| Microcontroller | Arduino Nano (or compatible) |
| LED strip | SK6812 RGBW, 73+ LEDs |
| Power supply | 5 V, 3 A+ |
| Connection | Shared GND between PSU and Arduino |

## 🔌 Wiring

1. LED **data** → Arduino **pin 9**
2. LED **VCC** → external 5V PSU
3. LED **GND** → PSU GND **and** Arduino GND
4. Arduino → PC via USB

> ⚠️ Never power the LED strip from the Arduino 5V pin — use the external PSU.

---

## 🔼 Firmware Upload

From the `Ambilight` folder:

```powershell
pio run -t upload
```

Firmware defaults (`src/main.cpp`):

| Setting | Value |
|---------|-------|
| LEDs | 73 |
| Serial baud | 115200 |
| LED pin | 9 |
| Protocol | Adalight |
| Idle timeout | 3 s (warm amber) |
| Off timeout | 10 min (fade out) |

---

## 🐍 Python Sender — Quick Start

> Easiest to set up. Uses DXcam (DirectX 11 desktop duplication).

```powershell
cd pc_app
pip install -r requirements.txt
python ambilight.py --port COM3 --monitor 0 --fps 30 --profile
```

### Examples

```powershell
# Minimal — auto-detect port, primary monitor, 30 FPS
python ambilight.py

# Specific COM port and second monitor
python ambilight.py --port COM5 --monitor 1

# Gaming — max FPS, no smoothing
python ambilight.py --fps 60 --smoothing 1.0

# Ambient / low-power — lower FPS, light smoothing
python ambilight.py --fps 20 --smoothing 0.7

# Skip identical frames to save CPU
python ambilight.py --frame-skip-threshold 5 --frame-skip-stride 16

# Auto-idle when screen is static
python ambilight.py --idle-fps 5 --idle-seconds 3.0

# See profiling breakdown
python ambilight.py --profile

# List all COM ports
python ambilight.py --list-ports
```

See `pc_app/README.md` for full option reference.

---

## ⚡ Native C++ Sender — Quick Start

> Lowest CPU usage. GPU compute shader averages regions — only ~1 KB readback per frame.

```powershell
cd pc_app_native

# Configure (once)
"C:\Program Files\CMake\bin\cmake.exe" --preset vs2026-release

# Build
"C:\Program Files\CMake\bin\cmake.exe" --build --preset vs2026-release

# Run
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60 --monitor 0 --profile
```

> 📌 Firmware default baud is **115200** — always pass `--baud 115200` unless you change the firmware.

### Examples

```powershell
# Standard — COM3, primary monitor, 60 FPS
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60

# With profiling to see per-stage timings
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60 --profile

# Second monitor
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --monitor 1 --fps 60

# Reduce compute load (sample every 4th pixel)
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --downscale 4

# Smooth transitions (0.3 = heavy smooth, 1.0 = off)
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --smoothing 0.3

# List COM ports
.\build-vs\Release\ambilight_native.exe --list-ports
```

See `pc_app_native/README.md` for full build and option reference.

---

## 🆚 Sender Comparison

| Feature | Python | Native C++ |
|---------|--------|------------|
| Setup effort | pip install | CMake + MSVC |
| Capture API | DXcam (DXGI) | DXGI Desktop Duplication |
| Region averaging | NumPy (CPU) | GPU compute shader |
| GPU→CPU transfer | Full frame RGB | ~1 KB (73 float4s) |
| CPU usage | Very low | Near zero |
| Platform | Any Python 3 | Windows only |

---

## 🔧 Troubleshooting

### ❌ No LEDs / wrong port
- Run with `--list-ports` to see available COM ports
- Check Device Manager on Windows

### ❌ `d3d11.h: no include path set`
- Use the preset-based build in `pc_app_native/README.md` — it sets up VS environment automatically

### ❌ Flickering or too much CPU
- Lower `--fps`
- Add `--smoothing 0.5`
- Increase `--downscale 4`

### ❌ Native exe stuck at 60 FPS
- Expected at 60 Hz — desktop duplication delivers at monitor refresh rate
- Get a 144 Hz monitor for higher unique frame rates

### ❌ Upload fails
- Disconnect the sender app before uploading firmware
- Try baud rate 57600 in `platformio.ini` for upload

---

## 📡 Protocol Reference

Adalight frame format sent each render:

| Bytes | Content |
|-------|---------|
| 0–2 | `Ada` (ASCII header) |
| 3 | LED count high byte (`LEDS_TOTAL − 1`) |
| 4 | LED count low byte |
| 5 | Checksum: `hi ^ lo ^ 0x55` |
| 6+ | RGB payload: 73 LEDs × 3 bytes = **219 bytes** |

Arduino responds with `Ada\n` on startup as a ready signal.
