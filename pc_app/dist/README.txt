====================================
    AMBILIGHT - Standalone Executable
====================================

This is a standalone executable for the Ambilight system.
No Python installation required!

QUICK START:
-----------
1. Connect your Arduino with Ambilight firmware
2. Double-click Ambilight.exe
3. The app will auto-detect your Arduino and start

COMMAND LINE OPTIONS:
--------------------
Open Command Prompt in this folder and run:

  Ambilight.exe --help              Show all options
  Ambilight.exe --list-ports        List available serial ports
  Ambilight.exe --port COM3         Use specific port
  Ambilight.exe --fps 20            Lower CPU usage (20 FPS)
  Ambilight.exe --fps 45            Higher FPS for gaming
  Ambilight.exe --brightness 150    Reduce brightness
  Ambilight.exe --saturation 1.5    Boost color saturation
  Ambilight.exe --smoothing 0.7     Smoother transitions

EXAMPLES:
---------
# Lower CPU usage
Ambilight.exe --fps 20

# Specific COM port and monitor
Ambilight.exe --port COM3 --monitor 1

# Smoother, dimmer lighting
Ambilight.exe --brightness 150 --smoothing 0.7

TROUBLESHOOTING:
---------------
- If Arduino not detected: Use --list-ports to find the correct port
- If CPU usage is high: Lower --fps to 20
- To stop: Press Ctrl+C in the window

FILE SIZE: ~20 MB (includes Python, numpy, and all dependencies)

For more info, see the main README.md in the project folder.
