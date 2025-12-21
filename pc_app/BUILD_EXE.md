# Building Ambilight.exe from Python Source

This guide explains how to create a standalone executable from the Python script using PyInstaller.

## Prerequisites

- Python 3.7 or higher installed
- Git (optional, for cloning the repository)
- Windows 7 or later

## Installation & Build Steps

### 1. Install Python Dependencies

Navigate to the `pc_app` folder and install required packages:

```bash
cd pc_app
pip install -r requirements.txt
```

This installs:
- `numpy` - Array operations
- `mss` - Screen capture
- `pyserial` - Serial communication
- `psutil` - Process priority management (optional)

### 2. Install PyInstaller

```bash
pip install pyinstaller
```

### 3. Build the Executable

Run one of these commands:

**Option A: Single EXE file (recommended)**
```bash
python -m PyInstaller --onefile --name "Ambilight" --console ambilight.py
```

**Option B: With custom icon (optional)**
```bash
python -m PyInstaller --onefile --name "Ambilight" --console --icon=ambilight.ico ambilight.py
```

**Option C: Windowed app (no console)**
```bash
python -m PyInstaller --onefile --name "Ambilight" --windowed ambilight.py
```

### 4. Find Your Executable

The compiled `.exe` will be in:
```
pc_app\dist\Ambilight.exe
```

## Rebuilding

If you've modified `ambilight.py` or the dependencies, rebuild using:

```bash
python -m PyInstaller Ambilight.spec
```

This reuses the existing spec file and is faster than rebuilding from scratch.

## Customization

### Add an Icon

Create a custom icon (`.ico` file) and use:

```bash
python -m PyInstaller --onefile --name "Ambilight" --console --icon=myicon.ico ambilight.py
```

### Change Executable Name

Replace `"Ambilight"` with your desired name:

```bash
python -m PyInstaller --onefile --name "MyAmbilightApp" --console ambilight.py
```

### Hidden Imports (if needed)

If modules fail to import, add them manually:

```bash
python -m PyInstaller --onefile --name "Ambilight" --console --hidden-import=psutil ambilight.py
```

### Reduce File Size

Strip unnecessary data:

```bash
python -m PyInstaller --onefile --name "Ambilight" --console --strip ambilight.py
```

## Generated Files

After building, you'll have:

```
pc_app/
├── Ambilight.spec          # Build specification (reusable)
├── build/                  # Temporary build files
├── dist/
│   └── Ambilight.exe       # ✓ Your executable
├── ambilight.py            # Source code
└── requirements.txt
```

You can safely delete `build/` folder to save space.

## Troubleshooting

### "PyInstaller not found"
Make sure it's installed:
```bash
pip install pyinstaller
```

### Missing module errors
Install the missing package:
```bash
pip install <module_name>
```

Then rebuild:
```bash
python -m PyInstaller Ambilight.spec
```

### Executable too large (>50 MB)
This is normal - it includes Python runtime and all dependencies. Options to reduce:
- Remove unused dependencies from `requirements.txt`
- Use `--strip` flag during build
- Remove debug symbols: `--nouptime`

### "Access denied" when running exe
- Close any antivirus or security software
- Run from a different folder
- Try rebuilding

### Build takes very long
This is normal on first build. Subsequent builds using `.spec` file are faster.

## Distribution

To share the executable:

1. **Standalone (recommended):**
   - Copy just `dist/Ambilight.exe`
   - No Python installation needed

2. **With source:**
   - Include `README.md` and `Ambilight.exe`
   - Users can modify and rebuild if needed

3. **Installer (advanced):**
   - Use NSIS or Inno Setup to create a Windows installer
   - Allows easy installation to Program Files

## Advanced: Creating a Spec File Manually

If you want fine-grained control, create a custom spec:

```bash
pyi-makespec --onefile --name "Ambilight" --console ambilight.py
```

Then edit `Ambilight.spec` and rebuild:

```bash
python -m PyInstaller Ambilight.spec
```

## Performance Notes

- First build: 1-3 minutes (analyzes dependencies)
- Subsequent builds: 30-60 seconds
- File size: ~20 MB (includes Python 3.13 + numpy)

For faster iteration during development, use the Python script directly:

```bash
python ambilight.py --fps 30
```

## Updating the Executable

To rebuild with latest code:

1. Make changes to `ambilight.py`
2. Update dependencies if needed in `requirements.txt`
3. Rebuild:
   ```bash
   python -m PyInstaller Ambilight.spec
   ```
4. New `Ambilight.exe` will be in `dist/` folder

## References

- [PyInstaller Documentation](https://pyinstaller.org/)
- [Requirements.txt in this project](requirements.txt)
- [Ambilight Python source](ambilight.py)
