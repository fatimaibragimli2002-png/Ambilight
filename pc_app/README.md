# 🐍 Ambilight Python Sender

Python-based Ambilight sender using DXcam (DirectX 11 Desktop Duplication) with serial Adalight output.

---

## ⚙️ Setup

```powershell
cd pc_app
pip install -r requirements.txt
```

---

## ▶️ Run

```powershell
python ambilight.py --port COM3 --monitor 0 --fps 30 --profile
```

---

## 📋 Options

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | `COM3` | Serial COM port |
| `--monitor` | `0` | Monitor index (0-based) |
| `--fps` | `30` | Target frames per second |
| `--smoothing` | `1.0` | EMA factor: `1.0` = off, `0.3` = heavy smooth |
| `--downscale` | `1` | Downsample before processing |
| `--frame-skip-threshold` | `0` | Skip frames with mean diff below threshold |
| `--frame-skip-stride` | `16` | Sample step for frame diff check |
| `--idle-fps` | `0` | Reduced FPS when idle (`0` = disabled) |
| `--idle-seconds` | `2.0` | Seconds of static content before idle mode |
| `--profile` | off | Print per-stage timing breakdown |
| `--list-ports` | — | Print COM ports and exit |

---

## 💡 Examples

```powershell
# Default — auto-detect port, primary monitor, 30 FPS
python ambilight.py

# Specific port and second monitor
python ambilight.py --port COM5 --monitor 1

# Gaming — max FPS, no smoothing
python ambilight.py --fps 60 --smoothing 1.0

# Ambient / low-power — slow FPS, light smoothing
python ambilight.py --fps 20 --smoothing 0.7

# Skip nearly-identical frames to cut CPU spikes
python ambilight.py --frame-skip-threshold 5 --frame-skip-stride 16

# Auto-reduce to 5 FPS after 3 s of no screen change
python ambilight.py --idle-fps 5 --idle-seconds 3.0

# Show profiling overlay every 2 s
python ambilight.py --profile

# List available serial ports
python ambilight.py --list-ports
```

---

## 📊 Profile Output

With `--profile`, the console shows a live status line:

```
FPS: 30.01 | capture:8.2ms | sampling:1.3ms | serial:1.1ms
```

---

## 📦 Build Standalone EXE

See `BUILD_EXE.md` for full PyInstaller instructions.

Quick version:

```powershell
pip install pyinstaller
python -m PyInstaller --onefile --name "Ambilight" --console ambilight.py
# Output: dist\Ambilight.exe
```
