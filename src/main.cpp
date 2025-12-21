/*
 * Ambilight Arduino Firmware for SK6812 RGBW
 *
 * LED Layout: 73 LEDs total
 * - Left side:  19 LEDs (bottom to top)
 * - Top side:   35 LEDs (left to right)
 * - Right side: 19 LEDs (top to bottom)
 *
 * Protocol (Adalight):
 * - Header: "Ada" (3 bytes)
 * - LED count high byte, low byte
 * - Checksum (high XOR low XOR 0x55)
 * - RGB data for each LED (3 bytes per LED) - converted to RGBW on Arduino
 */

#include <Arduino.h>
#include <FastLED.h>
#include "FastLED_RGBW.h"

// LED Configuration
#define NUM_LEDS 73  // Total LEDs: 19 + 35 + 19
#define LED_PIN 9    // Data pin connected to LED strip
#define BRIGHTNESS 255

// Serial Configuration
#define SERIAL_RATE 115200  // High speed serial (was 115200)
#define IDLE_TIMEOUT 5000   // ms before showing ambient color
#define OFF_TIMEOUT 600000  // ms (10 min) before turning off

// LED buffer: CRGBW array and RGB pointer for FastLED hack
CRGBW leds[NUM_LEDS];
CRGB* ledsRGB = (CRGB*)&leds[0];

// Protocol variables
uint8_t prefix[] = {'A', 'd', 'a'};
uint8_t hi, lo, chk;

// Timing
unsigned long lastDataTime = 0;
uint8_t currentBrightness = BRIGHTNESS;

void setup() {
  // Initialize FastLED with RGBW hack
  FastLED.addLeds<WS2812B, LED_PIN, RGB>(ledsRGB, getRGBWsize(NUM_LEDS));
  FastLED.setBrightness(currentBrightness);
  // No refresh rate limit - go as fast as possible

  Serial.begin(SERIAL_RATE);
  delay(100);

  // Clear
  memset(leds, 0, NUM_LEDS * sizeof(CRGBW));
  FastLED.show();

  // Send ready signal
  Serial.print("Ada\n");
}

void checkConnection() {
  unsigned long elapsed = millis() - lastDataTime;

  if (elapsed > OFF_TIMEOUT) {
    // Long idle: fade to off
    if (currentBrightness > 0) {
      currentBrightness--;
      FastLED.setBrightness(currentBrightness);
      FastLED.show();
      delay(50);
    }
  } else if (elapsed > IDLE_TIMEOUT) {
    // Short idle: show ambient warm color
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] = CRGB::SaddleBrown;  // Warm ambient
    }
    FastLED.show();
  }
}

void loop() {
  uint8_t i;

  // Wait for "Ada" prefix
  for (i = 0; i < sizeof(prefix); ++i) {
  waitLoop:
    while (!Serial.available())
      checkConnection();

    if (prefix[i] == Serial.read())
      continue;
    i = 0;
    goto waitLoop;
  }

  // Read LED count (high byte)
  while (!Serial.available())
    checkConnection();
  hi = Serial.read();

  // Read LED count (low byte)
  while (!Serial.available())
    checkConnection();
  lo = Serial.read();

  // Read checksum
  while (!Serial.available())
    checkConnection();
  chk = Serial.read();

  // Verify checksum
  if (chk != (hi ^ lo ^ 0x55)) {
    i = 0;
    goto waitLoop;
  }

  // Calculate number of LEDs to read
  uint16_t numLeds = ((uint16_t)hi << 8) | lo;
  numLeds++;  // Protocol sends count-1

  if (numLeds > NUM_LEDS) {
    numLeds = NUM_LEDS;
  }

  // Clear LED buffer
  memset(leds, 0, NUM_LEDS * sizeof(CRGBW));

  // Read RGB data for each LED (3 bytes per LED)
  for (uint16_t ledIdx = 0; ledIdx < numLeds; ledIdx++) {
    uint8_t r, g, b;

    while (!Serial.available())
      checkConnection();
    r = Serial.read();

    while (!Serial.available())
      checkConnection();
    g = Serial.read();

    while (!Serial.available())
      checkConnection();
    b = Serial.read();

    // Convert RGB to RGBW
    // Extract white component from minimum of R,G,B
    uint8_t w = min(min(r, g), b);

    // Subtract white from RGB channels
    leds[ledIdx].r = r - w;
    leds[ledIdx].g = g - w;
    leds[ledIdx].b = b - w;
    leds[ledIdx].w = w;
  }

  // Update LEDs
  lastDataTime = millis();
  currentBrightness = BRIGHTNESS;
  FastLED.setBrightness(currentBrightness);
  FastLED.show();
}