# Ambilight System - CPU Optimized

DIY ambient lighting system for your monitor using Arduino and RGBW addressable LEDs.

## Features

- ✨ **RGBW Support** - SK6812 RGBW LEDs with white channel
- 🚀 **CPU Optimized** - Captures only screen edges (not full screen)
- ⚡ **Fast & Smooth** - 30 FPS default with low system impact
- 🎨 **Color Enhancement** - Adjustable saturation and brightness
- 🔄 **Temporal Smoothing** - Eliminates flickering
- 🔧 **Low Priority Mode** - Automatically yields to other apps

## LED Layout

```
          ┌───────────────────────────────────────┐
          │             TOP (35 LEDs)             │
          │           ← left to right →           │
      ┌───┼─────────────────────────────────────┬─┼─┐
      │ L │                                     │ R │
      │ E │                                     │ I │
      │ F │                                     │ G │
      │ T │               MONITOR               │ H │
      │   │                                     │ T │
      │ 19│                                     │ 19│
      │   │                                     │   │
      │ ↑ │                                     │ ↓ │
      └─┬─┴─────────────────────────────────────┴─┬─┘
        │                                         │
      START                                      END
  (bottom left)                            (bottom right)
```

Total: 73 LEDs (19 left + 35 top + 19 right)

## Hardware Requirements

- Arduino Nano (or compatible)
- SK6812 RGBW addressable LED strip (73+ LEDs)
- 5V power supply (at least 3A recommended for 73 LEDs)
- USB cable for Arduino

> **Note**: This project is optimized for SK6812 RGBW LEDs. For standard RGB LEDs (WS2812B), you'll need to modify the Arduino code.

## Wiring

1. **LED Data Pin** → Arduino Pin 6
2. **LED VCC** → 5V Power Supply (+)
3. **LED GND** → 5V Power Supply (-) AND Arduino GND
4. **Arduino** → USB to PC 

> ⚠️ **Important**: Don't power more than a few LEDs directly from Arduino 5V pin. Use an external 5V power supply for the LED strip.

## Arduino Setup (or Arduino IDE)
2. Ensure `include/FastLED_RGBW.h` is present (RGBW support hack)
3. Verify the LED configuration in `src/main.cpp`:
   - `LED_PIN`: Data pin (default: 6)
   - `NUM_LEDS`: Total LED count (default: 73)
   - Serial baud: **115200** (must match PC app)
4. Build and upload to Arduino
5. Open Serial Monitor - wait for "Ada" message (ready signal)

> **Baud Rate**: Both Arduino and PC app use **115200 baud**. Higher rates may cause upload issues. (default: WS2812B)
   - `COLOR_ORDER`: Color order (default: GRB)
3. Build and upload to Arduino

## PC Application Setup

### Install Python Dependencies

```bash
cd pc_app
pip install -r requirements.txt
```

### Run the Application

```bash
python ambilight.py
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--port`, `-p` | Serial port (e.g., COM3) | Auto-detect |
| `--monitor`, `-m` | Monitor number | 0 (primary) |
| `--smoothing` | Temporal smoothing (0.0-1.0) | 0.6 |
| `--fps`, `-f` | Target frames per second | 30 |
| `--brightness`, `-b` | LED brightness (0-255) | 255 |
| `--saturation`, `-s` | Color saturation boost | 1.2 |
| `--list-ports` | List available serial ports | - |

### Examples

```bash, 30 FPS (recommended)
python ambilight.py

# Specific port and monitor
python ambilight.py --port COM3 --monitor 1

# Lower CPU usage (20 FPS)
python ambilight.py --fps 20

# Smoother transitions (higher smoothing)
python ambilight.py --smoothing 0.7

# Higher FPS for gaming (max recommended: 45)
pytPerformance & Optimization

### CPU Usage
The PC app is highly optimized:
- **Captures only screen edges** (not full screen) - ~90% less data
- **Low process priority** - automatically yields CPU to other apps
- **30 FPS default** - smooth while being CPU-friendly
- **Pre-computed segments** - no redundant calculations

### Tuning for Your System

**For lower CPU usage:**
```bash
python ambilight.py --fps 20 --smoothing 0.7
```

**For smoother/faster response:**
```bash
python ambilight.py --fps 45 --smoothing 0.4
```

**Recommended FPS ranges:**
- 20 FPS: Minimal CPU, still smooth for videos
- 30 FPS: Balanc30  # Pixels from screen edge to sample (smaller = faster)
```

### Smoothing & Response

Adjust temporal smoothing for your preference:
- `--smoothing 0.3`: Very smooth, slower response
- `--smoothing 0.6`: Balanced (default)
- `--smoothing 0.8`: Fast response, may flicker slightlyist available serial ports
python ambilight.py --list-ports
```

## Customization

### Changing LED Count

If your LED setup is different, update both files:

1. **Ardushow only white or wrong colors
- This project uses RGBW LEDs (SK6812) with custom FastLED_RGBW.h
- Arduino receives RGB and converts to RGBW internally
- Check that `FastLED_RGBW.h` is in the `include/` folder

### LEDs don't light up
- Check wiring connections (Data to Pin 6, GND to GND)
- Verify power supply is adequate (3A+ for 73 LEDs)
- Open Serial Monitor at 115200 baud - look for "Ada" message

### Upload fails / "Arduino not responding"
- Disconnect PC app before uploading
- Use upload speed 57600 (set in platformio.ini)
- If using high baud rate, revert to 115200

### Serial connection fails
- Check if Arduino is connected: `python ambilight.py --list-ports`
- On Windows: Check Device Manager for COM port
- On Linux: Add user to dialout group: `sudo usermod -a -G dialout $USER`

### High CPU usage / PC slowdown
- Lower FPS: `python ambilight.py --fps 20`
- Increase smoothing: `python ambilight.py --smoothing 0.7`
- TTechnical Details

### RGBW Support
This project uses SK6812 RGBW LEDs with a FastLED hack:
- Arduino receives **RGB** data (3 bytes per LED)
- Converts to **RGBW** internally using FastLED_RGBW.h
- Extracts white channel: `W = min(R, G, B)`
- See `include/FastLED_RGBW.h` for implementation

### Baud Rate
- Fixed at **115200 baud**
- Higher rates (500000) cause upload conflicts
- Sufficient for 73 LEDs @ 30 FPS

### Protocol

The system uses a simplified Adalight protocol:

| Byte | Description |
|------|-------------|
| 0-2 | Header: "Ada" |
| 3 | LED count high byte |
| 4 | LED count low byte |
| 5 | Checksum (hi ^ lo ^ 0x55) |
| 6+ | RGB data (3 bytes per LED) |

Arduino responds with "Ada\n" on startup for handshake.

### LEDs don't light up
- Check wiring connections
- Verify power supply is adequate
- Ensure correct LED_TYPE and COLOR_ORDER in code

### Colors are wrong
- Try different COLOR_ORDER values: GRB, RGB, BRG, etc.
- Check LED_TYPE matches your strip

### Flickering
- Lower the `--fps` value
- Increase smoothing in the Python script

### Serial connection fails
- Check if Arduino is connected
- Use `--list-ports` to see available ports
- On Windows, check Device Manager for correct COM port
- On Linux, you may need to add user to `dialout` group

### High CPU usage
- Lower the `--fps` value
- Increase CAPTURE_DEPTH (larger areas sample faster)

## Protocol

The system uses a simplified Adalight protocol:

| Byte | Description |
|------|-------------|
| 0-2 | Header: "Ada" |
| 3 | LED count high byte |
| 4 | LED count low byte |
| 5 | Checksum (hi ^ lo ^ 0x55) |
| 6+ | RGB data (3 bytes per LED) |

## License

MIT License - Feel free to modify and share!
