// =====================================================================================
// display.h
// SmartThermo: the Adafruit 1.44" 128x128 TFT (product 2088, ST7735R driver) on the RP2040's
// second SPI bus, showing the room temperature, the setpoint and the fan, run and mode icons. Runs
// on core 0, the core that runs the HTTP server; core 1 never touches the display, and stateMutex
// is only held to read the thermostat, so a draw or a fade can't hold up the HVAC.
// =====================================================================================
#pragma once

#include "common.h"

// ---- GPIO pins ----
// SPI1 and not SPI: the core's default SPI0 pins are GP16, GP18 and GP19, and starting that bus
// would take GP16 (G relay) and GP18 (W1 relay) away from the thermostat. The display only
// receives, so SPI1's RX pin (GP12 by default) is released and used for D/C instead.
const uint8_t DISPLAY_LITE_PIN = 10; // GP10 = physical pin 14, Lite (green stripe)
const uint8_t DISPLAY_RST_PIN = 11;  // GP11 = physical pin 15, RST (red stripe)
const uint8_t DISPLAY_DC_PIN = 12;   // GP12 = physical pin 16, D/C (black stripe)
const uint8_t DISPLAY_CS_PIN = 13;   // GP13 = physical pin 17, TCS (blue stripe)
const uint8_t DISPLAY_SCK_PIN = 14;  // GP14 = physical pin 19, SCK clock (shielded black)
const uint8_t DISPLAY_MOSI_PIN = 15; // GP15 = physical pin 20, SI TX (shielded red)

// ---- backlight brightness ----
// Levels are how bright it looks, not PWM duty; display.cpp converts them.
const uint8_t DISPLAY_BRIGHTNESS_MAX = 255;     // full; 0 is off
const uint8_t DISPLAY_BRIGHTNESS_DEFAULT = 180; // about 70%, the level after every start

// ---- text colours: the temperatures' colour is a place in this table, set with ?7,0,N~ ----
struct DisplayTextColor {
  const char *name;
  uint16_t rgb565;
};
const DisplayTextColor DISPLAY_TEXT_COLORS[] = {
  { "white", 0xFFFF },   // 0
  { "silver", 0xC618 },  // 1
  { "red", 0xF800 },     // 2
  { "orange", 0xFBC0 },  // 3
  { "amber", 0xFDE0 },   // 4
  { "yellow", 0xFFE0 },  // 5
  { "lime", 0xAFE0 },    // 6
  { "green", 0x07E0 },   // 7
  { "teal", 0x0595 },    // 8
  { "cyan", 0x07FF },    // 9
  { "sky", 0x5D7F },     // 10
  { "blue", 0x2B7F },    // 11
  { "purple", 0xA29F },  // 12
  { "magenta", 0xF81F }, // 13
  { "pink", 0xFC17 },    // 14
};
const uint8_t DISPLAY_TEXT_COLOR_COUNT = sizeof(DISPLAY_TEXT_COLORS) / sizeof(DISPLAY_TEXT_COLORS[0]);
const uint16_t DISPLAY_TEXT_COLOR_DEFAULT = 0; // white
static_assert(DISPLAY_TEXT_COLOR_COUNT == 15, "the API documents 15 text colours");

// ---- temperature font: set DISPLAY_FONT to one of these and rebuild ----
const uint8_t DISPLAY_FONT_JOSEFIN_SANS = 0;   // Josefin Sans SemiBold, the website's font
const uint8_t DISPLAY_FONT_FREE_SANS_BOLD = 1; // FreeSansBold, larger and heavier
const uint8_t DISPLAY_FONT = DISPLAY_FONT_JOSEFIN_SANS;

// ---- image position ----
// How the panel sits in the enclosure, with the DHT22 on the left. Rotation is the library's
// setRotation() value, 0-3. The offsets move the image's centre from the panel's centre to the
// centre of the circular cutout, in pixels after rotation: +X is right, +Y is down, 1 pixel is
// about 0.2 mm. Set from a photo of the enclosure, in which the cutout shows about 110 pixels across.
const uint8_t DISPLAY_ROTATION = 1;
const int8_t DISPLAY_OFFSET_X = -1;
const int8_t DISPLAY_OFFSET_Y = 4;

void startDisplay();
void updateDisplay();
void wakeDisplay();
void setDisplayDefaultBrightness(uint8_t level);
uint8_t displayDefaultBrightness();
