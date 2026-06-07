# ⚡ Ambilight Native Sender (C++)

High-performance Windows sender for Ambilight.
Uses DXGI Desktop Duplication + D3D11 GPU compute shader — region averaging runs entirely on GPU, reading back only ~1 KB per frame instead of 8 MB.

---

## 🏗️ Architecture

| Stage | Implementation |
|-------|---------------|
| Screen capture | DXGI Desktop Duplication |
| Region averaging | D3D11 compute shader (GPU) |
| GPU→CPU transfer | ~1 KB (73 × float4) |
| Serial output | Win32 serial, Adalight protocol |
| Frame pacing | High-resolution waitable timer |
| Timer resolution | `timeBeginPeriod(1)` |

---

## 🔧 Requirements

- Windows 10 / 11
- Visual Studio 2026 Build Tools (MSVC + Windows SDK 10.0.26100+)
- CMake 3.23+

---

## 🔨 Build

The project ships with `CMakePresets.json` (preset: `vs2026-release`). No need to open a Developer shell — the VS generator handles the environment automatically.

**Configure** (once, or after CMakeLists changes):

```powershell
"C:\Program Files\CMake\bin\cmake.exe" --preset vs2026-release
```

**Build** (repeat after any code change):

```powershell
"C:\Program Files\CMake\bin\cmake.exe" --build --preset vs2026-release
```

Output executable: `build-vs\Release\ambilight_native.exe`

> 💡 If `cmake` is already in your PATH, drop the full path prefix.

---

## ▶️ Run

```powershell
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60 --monitor 0 --profile
```

> ⚠️ Arduino firmware default baud is **115200** — always pass `--baud 115200` unless you changed the firmware.

### Examples

```powershell
# Minimal — COM3, primary monitor, 60 FPS
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60

# With profiling overlay
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --fps 60 --profile

# Second monitor
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --monitor 1

# Smooth transitions
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --smoothing 0.3

# Reduce GPU sampling load (every 4th pixel)
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --downscale 4

# Narrow edge capture zone (30 px deep)
.\build-vs\Release\ambilight_native.exe --port COM3 --baud 115200 --edge-depth 30

# List COM ports and exit
.\build-vs\Release\ambilight_native.exe --list-ports
```

---

## ⚙️ Options

All defaults live in one config block — `AppConfig` in `src/main.cpp`.

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | `COM3` | Serial COM port |
| `--monitor` | `0` | Monitor index (0-based) |
| `--fps` | `60` | Target frames per second |
| `--baud` | `460800` | Serial baud rate |
| `--edge-depth` | `50` | Pixels sampled from each edge |
| `--downscale` | `4` | Sample every N-th pixel (GPU shader step) |
| `--smoothing` | `1.0` | EMA factor: `1.0` = off, `0.3` = heavy smooth |
| `--profile` | off | Print per-stage timing every 2 s |
| `--list-ports` | — | Print COM ports and exit |

---

## 📊 Profile Output

With `--profile` the status line auto-refreshes every 2 seconds:

```
FPS: 59.97 | gpu:2.841ms | processing:0.089ms | serial:1.203ms | frame:4.133ms (24.8% budget)
```

| Field | Meaning |
|-------|---------|
| `fps` | Measured frames per second |
| `gpu` | Capture + compute shader dispatch + readback |
| `processing` | Smoothing pass |
| `serial` | Serial write (skipped if frame unchanged) |
| `frame` | Total loop time before frame-pacing sleep |
| `% budget` | `frame / (1000 / fps) × 100` — how much of budget consumed |

Durations auto-switch between `ms` and `s`.

---

## 🔧 Troubleshooting

### ❌ `d3d11.h: no include path set`

The shell is missing VC++ include paths. Fix: use the preset build command above — VS generator sets up the environment automatically.

### ❌ `LNK1168: cannot open ambilight_native.exe for writing`

The exe is still running. Stop it, then build again.

### ❌ Stuck at ~60 FPS

Desktop Duplication delivers frames at the monitor refresh rate. On a 60 Hz monitor, 60 unique frames/s is the hardware maximum. A 144 Hz monitor gives up to 144 FPS.
