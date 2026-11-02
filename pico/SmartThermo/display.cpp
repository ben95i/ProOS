// =====================================================================================
// display.cpp
// SmartThermo: starts the ST7735R TFT on SPI1 and draws the thermostat screen on it. See display.h.
//
// The screen is drawn into a 16-bit image in RAM and sent to the panel in one piece, so a change
// never flickers. The image is only as big as the circular cutout and its centre is the cutout's
// centre, so the layout below doesn't depend on the panel's rotation or DISPLAY_OFFSET_X / Y.
// =====================================================================================
#include <math.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include "JosefinSansDigits.h"
#include "display.h"
#include "thermo.h"

// ---- the image ----
const int16_t DISPLAY_IMAGE_SIZE = 112; // pixels across; the cutout shows about 110
const uint8_t DISPLAY_TEXT_SIZE = 12;   // a temperature as text, e.g. "-40"

// ---- the backlight's fade ----
const uint64_t BACKLIGHT_WAKE_HOLD_MS = 30000; // full brightness lasts this long after a setting changes
const uint64_t BACKLIGHT_FADE_STEP_MS = 8;     // one level per step: 0 to full takes about 2 s

// ---- the backlight's PWM ----
const uint32_t BACKLIGHT_PWM_FREQUENCY_HZ = 3000; // past visible flicker; the shortest pulse is 81 ns, estimated long enough for the MOSFET
const uint16_t BACKLIGHT_PWM_RANGE = 4095; // duty for always on; the curve needs more than 8 bits at the dim end
const float BACKLIGHT_GAMMA = 2.8f;        // the eye sees duty as far brighter than it is: 30% duty looks about 65%

// ---- layout, in image pixels from its top left corner ----
const int16_t ROOM_TEXT_RIGHT_X = 65;      // large room temperature, with its text ending here
const int16_t ROOM_TEXT_MAX_WIDTH = 58;    // two digits; anything wider uses the setpoint's font
const int16_t ROOM_DEGREE_X = 69;
const int16_t ROOM_DEGREE_Y = 60;
const int16_t ROOM_DEGREE_RADIUS = 3;
const int16_t SETPOINT_TEXT_RIGHT_X = 41;  // small setpoint temperature
const int16_t SETPOINT_TEXT_BASELINE_Y = 45;
const int16_t SETPOINT_DEGREE_X = 45;
const int16_t SETPOINT_DEGREE_Y = 28;
const int16_t SETPOINT_DEGREE_RADIUS = 2;
const int16_t FAN_DISC_X = 62;
const int16_t FAN_DISC_Y = 37;
const int16_t RUN_DISC_X = 87;
const int16_t RUN_DISC_Y = 37;
const int16_t SMALL_DISC_RADIUS = 10;
const int16_t MODE_DISC_X = 86;
const int16_t MODE_DISC_Y = 75;
const int16_t MODE_DISC_RADIUS = 12;

// ---- colours (RGB565) ----
const uint16_t COLOR_BACKGROUND = 0x0000;
const uint16_t COLOR_ICON_LIT = 0xFFFF;   // white symbol of an icon that is on
const uint16_t COLOR_DISC_DIM = 0x2945;   // dark grey disc of an icon that is off
const uint16_t COLOR_ICON_DIM = 0x8C51;   // grey symbol of an icon that is off
const uint16_t COLOR_HEATING = 0xEA00;    // orange
const uint16_t COLOR_COOLING = 0x033F;    // blue
const uint16_t COLOR_BLOWER = 0x0500;     // green

// ---- icons: square, '#' is a lit pixel ----
const char ICON_PIXEL = '#';
const char *const FAN_ICON[] = {
  "...............",
  "....####.......",
  "...#####.......",
  "...#####....##.",
  "....####...###.",
  ".....###..####.",
  ".###..##.#####.",
  ".######.######.",
  ".#####.##..###.",
  ".####..###.....",
  ".###...####....",
  ".##....#####...",
  ".......#####...",
  ".......####....",
  "...............",
};
const char *const RUN_ICON[] = {
  "...............",
  "...............",
  "....##.........",
  "....####.......",
  "....#####......",
  "....#######....",
  "....########...",
  "....##########.",
  "....########...",
  "....#######....",
  "....#####......",
  "....####.......",
  "....##.........",
  "...............",
  "...............",
};
const char *const IDLE_ICON[] = {
  "...............",
  "...............",
  "...............",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...#########...",
  "...............",
  "...............",
  "...............",
};
const char *const HEAT_ICON[] = {
  ".................",
  ".........#.......",
  ".........##......",
  "........###......",
  "........####.....",
  ".......######....",
  "....#..######....",
  "...###########...",
  "...#####.#####...",
  "...####...####...",
  "...####....###...",
  "...###.....###...",
  "...###.....###...",
  "....###...###....",
  ".....#######.....",
  ".......###.......",
  ".................",
};
const char *const COOL_ICON[] = {
  "........#........",
  "......#.#.#......",
  ".......###.......",
  "..#.....#.....#..",
  "...##...#...##...",
  "..###...#...###..",
  ".....#..#..#.....",
  "......#.#.#......",
  "#################",
  "......#.#.#......",
  ".....#..#..#.....",
  "..###...#...###..",
  "...##...#...##...",
  "..#.....#.....#..",
  ".......###.......",
  "......#.#.#......",
  "........#........",
};
const char *const OFF_ICON[] = {
  "........#........",
  ".......###.......",
  ".......###.......",
  "...#...###...#...",
  "...##..###..##...",
  "..###..###..###..",
  ".###...###...###.",
  ".##....###....##.",
  ".##....###....##.",
  ".##...........##.",
  ".##...........##.",
  ".###.........###.",
  "..##.........##..",
  "..###.......###..",
  "...#####.#####...",
  "....#########....",
  "......#####......",
};

// A font for the temperatures, chosen with DISPLAY_FONT in display.h.
struct DisplayFont {
  const GFXfont *room;     // large room temperature
  const GFXfont *setpoint; // small setpoint temperature
  int16_t roomBaselineY;   // the large digits stand on this row of the image
};
// In the order of the DISPLAY_FONT_ values in display.h.
const DisplayFont DISPLAY_FONTS[] = {
  { &JosefinSansSemiBold47px, &JosefinSansSemiBold23px, 93 },
  { &FreeSansBold24pt7b, &FreeSansBold12pt7b, 91 },
};
const uint8_t DISPLAY_FONT_COUNT = sizeof(DISPLAY_FONTS) / sizeof(DISPLAY_FONTS[0]);
static_assert(DISPLAY_FONT < DISPLAY_FONT_COUNT, "DISPLAY_FONT must be one of the DISPLAY_FONT_ values");
const DisplayFont &shownFont = DISPLAY_FONTS[DISPLAY_FONT];

// What the screen shows. The room temperature follows the DHT22's samples, everything else changes at once.
struct DisplayState {
  bool hasTemperature;  // false while the sensor has no usable reading, shown as "--"
  int16_t temperatureF; // room temperature, whole degrees F
  uint16_t setpointF;   // whole degrees F
  uint16_t hvacMode;    // HVAC_MODE_OFF / HVAC_MODE_HEAT / HVAC_MODE_COOL
  int16_t running;      // RUNNING_IDLE / HEATING / COOLING / FAN_ONLY
  bool blowerRunning;   // G closed
  uint16_t textColor;   // place in DISPLAY_TEXT_COLORS
};

Adafruit_ST7735 displayPanel(&SPI1, DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);
GFXcanvas16 displayImage(DISPLAY_IMAGE_SIZE, DISPLAY_IMAGE_SIZE);
uint8_t backlightDefaultLevel = DISPLAY_BRIGHTNESS_DEFAULT; // level the backlight rests at
uint8_t backlightLevel = DISPLAY_BRIGHTNESS_DEFAULT;        // level on the Lite pin now
bool backlightWoken = false;                                // true while a setting change holds it at full
uint64_t backlightWokenAtMs = 0;                            // when a setting last changed
uint64_t backlightLastStepMs = 0;                           // when the fade last checked for a step
DisplayState shownState;                                 // what the panel shows now
uint32_t shownSensorSamples = 0;                         // DHT22 samples taken when the room temperature was last read

// Returns true when the given GPIO is one of the display's pins.
constexpr bool displayUsesPin(uint8_t pin) {
  bool usedAsPanelControl = (pin == DISPLAY_RST_PIN) || (pin == DISPLAY_DC_PIN) || (pin == DISPLAY_CS_PIN);
  bool usedAsControl = usedAsPanelControl || (pin == DISPLAY_LITE_PIN);
  bool usedAsBus = (pin == DISPLAY_SCK_PIN) || (pin == DISPLAY_MOSI_PIN);
  bool usesPin = usedAsControl || usedAsBus;
  return usesPin;
}

static_assert(!displayUsesPin(RELAY_FAN_G_PIN) && !displayUsesPin(RELAY_COOL_Y1_PIN) && !displayUsesPin(RELAY_HEAT_W1_PIN),
              "a display pin must never share a GPIO with a relay");
static_assert(!displayUsesPin(DHT22_DATA_PIN), "a display pin must never share a GPIO with the DHT22");

// Starts SPI1 on the display's pins with no receive pin, since the panel is write-only. The pins
// must be chosen before begin(); begin() runs here because the library only starts the buses it
// recognises, and running it a second time just sets the same pins again.
void startDisplayBus() {
  SPI1.setRX(NOPIN);
  SPI1.setSCK(DISPLAY_SCK_PIN);
  SPI1.setTX(DISPLAY_MOSI_PIN);
  SPI1.begin();
}

// Returns how far the font moves the cursor across the text, e.g. "71" in FreeSansBold24pt7b -> 52.
// FreeSansBold's digits all have the same advance, so its text stays still as the temperature changes;
// Josefin Sans's differ, so its tens digit moves with the units digit. The text may only hold
// characters the font has.
int16_t textAdvance(const char *text, const GFXfont *font) {
  int16_t advance = 0;
  for (size_t characterIndex = 0; text[characterIndex] != NULL_TERMINATOR; characterIndex++) {
    uint8_t character = (uint8_t)text[characterIndex];
    uint16_t glyphIndex = character - font->first;
    advance = advance + font->glyph[glyphIndex].xAdvance;
  }
  return advance;
}

// Draws text into the image with its advance ending at rightX, standing on baselineY.
void drawTextEndingAt(const char *text, const GFXfont *font, int16_t rightX, int16_t baselineY) {
  int16_t advance = textAdvance(text, font);
  int16_t cursorX = rightX - advance;
  displayImage.setFont(font);
  displayImage.setCursor(cursorX, baselineY);
  displayImage.print(text);
}

// Draws the large room temperature and its degree mark: e.g. 71, or "--" with no usable reading.
// A temperature too wide for the large digits, such as 102, uses the setpoint's font.
void drawRoomTemperature(const DisplayState &state) {
  char temperatureText[DISPLAY_TEXT_SIZE] = "--";
  if (state.hasTemperature) {
    snprintf(temperatureText, sizeof(temperatureText), "%d", state.temperatureF);
  }
  const GFXfont *font = shownFont.room;
  int16_t advance = textAdvance(temperatureText, font);
  bool tooWide = (advance > ROOM_TEXT_MAX_WIDTH);
  if (tooWide) {
    font = shownFont.setpoint;
  }
  drawTextEndingAt(temperatureText, font, ROOM_TEXT_RIGHT_X, shownFont.roomBaselineY);
  uint16_t textColor = DISPLAY_TEXT_COLORS[state.textColor].rgb565;
  displayImage.drawCircle(ROOM_DEGREE_X, ROOM_DEGREE_Y, ROOM_DEGREE_RADIUS, textColor);
}

// Draws the small setpoint temperature and its degree mark, e.g. 79.
void drawSetpoint(const DisplayState &state) {
  char setpointText[DISPLAY_TEXT_SIZE];
  snprintf(setpointText, sizeof(setpointText), "%u", state.setpointF);
  drawTextEndingAt(setpointText, shownFont.setpoint, SETPOINT_TEXT_RIGHT_X, SETPOINT_TEXT_BASELINE_Y);
  uint16_t textColor = DISPLAY_TEXT_COLORS[state.textColor].rgb565;
  displayImage.drawCircle(SETPOINT_DEGREE_X, SETPOINT_DEGREE_Y, SETPOINT_DEGREE_RADIUS, textColor);
}

// Draws a disc with an icon in its centre: litColor with a white icon while lit, dark grey with a
// grey icon while not, e.g. drawIconDisc(FAN_ICON, FAN_DISC_X, FAN_DISC_Y, 10, true, green).
void drawIconDisc(const char *const iconRows[], int16_t centerX, int16_t centerY, int16_t radius, bool lit, uint16_t litColor) {
  uint16_t discColor = COLOR_DISC_DIM;
  uint16_t iconColor = COLOR_ICON_DIM;
  if (lit) {
    discColor = litColor;
    iconColor = COLOR_ICON_LIT;
  }
  displayImage.fillCircle(centerX, centerY, radius, discColor);
  int16_t iconSize = (int16_t)strlen(iconRows[0]);
  int16_t iconHalf = iconSize / 2;
  int16_t iconLeft = centerX - iconHalf;
  int16_t iconTop = centerY - iconHalf;
  for (int16_t row = 0; row < iconSize; row++) {
    for (int16_t column = 0; column < iconSize; column++) {
      bool pixelIsLit = (iconRows[row][column] == ICON_PIXEL);
      if (pixelIsLit) {
        int16_t pixelX = iconLeft + column;
        int16_t pixelY = iconTop + row;
        displayImage.drawPixel(pixelX, pixelY, iconColor);
      }
    }
  }
}

// Draws the run icon: lit orange while heating or blue while cooling, or a dim square otherwise,
// including while only the blower runs.
void drawRunIcon(int16_t running) {
  switch (running) {
    case RUNNING_HEATING:
      drawIconDisc(RUN_ICON, RUN_DISC_X, RUN_DISC_Y, SMALL_DISC_RADIUS, true, COLOR_HEATING);
      break;
    case RUNNING_COOLING:
      drawIconDisc(RUN_ICON, RUN_DISC_X, RUN_DISC_Y, SMALL_DISC_RADIUS, true, COLOR_COOLING);
      break;
    default:
      drawIconDisc(IDLE_ICON, RUN_DISC_X, RUN_DISC_Y, SMALL_DISC_RADIUS, false, COLOR_ICON_DIM);
      break;
  }
}

// Draws the mode icon: a flame in heat, a snowflake in cool, a dim power symbol while off.
void drawModeIcon(uint16_t hvacMode) {
  switch (hvacMode) {
    case HVAC_MODE_HEAT:
      drawIconDisc(HEAT_ICON, MODE_DISC_X, MODE_DISC_Y, MODE_DISC_RADIUS, true, COLOR_HEATING);
      break;
    case HVAC_MODE_COOL:
      drawIconDisc(COOL_ICON, MODE_DISC_X, MODE_DISC_Y, MODE_DISC_RADIUS, true, COLOR_COOLING);
      break;
    default:
      drawIconDisc(OFF_ICON, MODE_DISC_X, MODE_DISC_Y, MODE_DISC_RADIUS, false, COLOR_ICON_DIM);
      break;
  }
}

// Draws the whole screen into the image and sends it to the panel, centred on the cutout. Takes
// about 10 ms on core 0, without stateMutex. Does nothing when the image's memory wasn't available.
void drawScreen(const DisplayState &state) {
  uint16_t *imagePixels = displayImage.getBuffer();
  bool imageMissing = (imagePixels == nullptr);
  if (imageMissing) {
    return;
  }
  displayImage.fillScreen(COLOR_BACKGROUND);
  displayImage.setTextColor(DISPLAY_TEXT_COLORS[state.textColor].rgb565);
  displayImage.setTextWrap(false);
  drawSetpoint(state);
  drawRoomTemperature(state);
  drawIconDisc(FAN_ICON, FAN_DISC_X, FAN_DISC_Y, SMALL_DISC_RADIUS, state.blowerRunning, COLOR_BLOWER);
  drawRunIcon(state.running);
  drawModeIcon(state.hvacMode);
  int16_t freeWidth = displayPanel.width() - DISPLAY_IMAGE_SIZE;
  int16_t freeHeight = displayPanel.height() - DISPLAY_IMAGE_SIZE;
  int16_t imageLeft = (freeWidth / 2) + DISPLAY_OFFSET_X;
  int16_t imageTop = (freeHeight / 2) + DISPLAY_OFFSET_Y;
  displayPanel.drawRGBBitmap(imageLeft, imageTop, imagePixels, DISPLAY_IMAGE_SIZE, DISPLAY_IMAGE_SIZE);
}

// Returns what the thermostat is doing now, taking stateMutex for the moment it reads. sensorSamples
// is set to the number of DHT22 samples taken so far, read in the same moment as the temperature.
DisplayState readDisplayState(uint32_t &sensorSamples) {
  DisplayState state;
  lockState();
  int16_t wholeTemperatureF = 0;
  state.hasTemperature = readWholeTemperatureF(wholeTemperatureF);
  state.temperatureF = wholeTemperatureF;
  state.setpointF = settings.setpointF;
  state.hvacMode = settings.hvacMode;
  state.running = runningState();
  RelayOutputs relays = currentRelays();
  state.blowerRunning = relays.fanG;
  state.textColor = settings.textColor;
  sensorSamples = sensorSampleCount();
  unlockState();
  return state;
}

// Returns true when both states would draw the same screen.
bool displayStatesMatch(const DisplayState &first, const DisplayState &second) {
  bool temperaturesMatch = (first.hasTemperature == second.hasTemperature)
                           && (first.temperatureF == second.temperatureF)
                           && (first.setpointF == second.setpointF);
  bool iconsMatch = (first.hvacMode == second.hvacMode)
                    && (first.running == second.running)
                    && (first.blowerRunning == second.blowerRunning);
  bool colorsMatch = (first.textColor == second.textColor);
  bool statesMatch = temperaturesMatch && iconsMatch && colorsMatch;
  return statesMatch;
}

// Returns the PWM duty for a backlight level, so equal steps in level look like equal steps in
// brightness: level ^ BACKLIGHT_GAMMA, e.g. of 4095: 255 -> 4095, 128 -> 595, 26 -> 7, and 0 -> 0.
// Any other level gets at least 1, so only level 0 is off.
uint16_t backlightDutyForLevel(uint8_t level) {
  if (level == 0) {
    return 0;
  }
  float levelFraction = (float)level / DISPLAY_BRIGHTNESS_MAX;
  float dutyFraction = powf(levelFraction, BACKLIGHT_GAMMA);
  float scaledDuty = dutyFraction * BACKLIGHT_PWM_RANGE;
  float roundedDuty = scaledDuty + 0.5f;
  uint16_t duty = (uint16_t)roundedDuty;
  if (duty < 1) {
    duty = 1;
  }
  return duty;
}

// Puts a backlight level on the Lite pin.
void writeBacklightLevel(uint8_t level) {
  uint16_t duty = backlightDutyForLevel(level);
  analogWrite(DISPLAY_LITE_PIN, duty);
}

// Moves the backlight one level towards where it belongs, at most once per BACKLIGHT_FADE_STEP_MS:
// full while a setting change holds it, otherwise the default level. A pass that comes late only
// slows the fade down, so the level never jumps.
void fadeBacklight() {
  uint64_t nowMs = uptimeMs();
  if (backlightWoken) {
    bool holdIsOver = intervalElapsed(backlightWokenAtMs, BACKLIGHT_WAKE_HOLD_MS, nowMs);
    if (holdIsOver) {
      backlightWoken = false;
    }
  }
  bool stepIsDue = intervalElapsed(backlightLastStepMs, BACKLIGHT_FADE_STEP_MS, nowMs);
  if (!stepIsDue) {
    return;
  }
  backlightLastStepMs = nowMs;
  uint8_t targetLevel = backlightDefaultLevel;
  if (backlightWoken) {
    targetLevel = DISPLAY_BRIGHTNESS_MAX;
  }
  if (backlightLevel == targetLevel) {
    return;
  }
  if (backlightLevel < targetLevel) {
    backlightLevel = backlightLevel + 1;
  } else {
    backlightLevel = backlightLevel - 1;
  }
  writeBacklightLevel(backlightLevel);
}

// Fades the backlight and redraws the screen when what it shows has changed; called from core 0's
// loop right after the HTTP server, so a setting change shows in the same pass. The setpoint, the
// icons and the text colour follow the thermostat at once; the room temperature only changes when
// a new DHT22 sample has been taken (every 5 s).
void updateDisplay() {
  fadeBacklight();
  uint32_t sensorSamples = 0;
  DisplayState latestState = readDisplayState(sensorSamples);
  bool sampleTaken = (sensorSamples != shownSensorSamples);
  if (sampleTaken) {
    shownSensorSamples = sensorSamples;
  } else {
    latestState.hasTemperature = shownState.hasTemperature;
    latestState.temperatureF = shownState.temperatureF;
  }
  bool unchanged = displayStatesMatch(latestState, shownState);
  if (unchanged) {
    return;
  }
  shownState = latestState;
  drawScreen(shownState);
}

// Fades the backlight up to full and holds it there for BACKLIGHT_WAKE_HOLD_MS, then lets it fade
// back to the default level; a further call restarts the hold. Called when a setting is adjusted,
// on core 0. Only sets the state updateDisplay() acts on, so it's safe with stateMutex held.
void wakeDisplay() {
  backlightWoken = true;
  backlightWokenAtMs = uptimeMs();
}

// Sets the level the backlight rests at, 0 (off) to DISPLAY_BRIGHTNESS_MAX (full), e.g. 128 looks
// about half as bright as full, and ends any hold so the backlight fades to the new level straight
// away. Core 0 only.
void setDisplayDefaultBrightness(uint8_t level) {
  backlightDefaultLevel = level;
  backlightWoken = false;
}

// Returns the level the backlight rests at, 0-255.
uint8_t displayDefaultBrightness() {
  return backlightDefaultLevel;
}

// Starts the panel, clears it, draws the first screen and sets the backlight to its default; called once
// from setup() on core 0, without stateMutex held. Takes about 1.2 s, mostly the panel's own reset
// and wake-up delays. The panel never answers, so a missing or unplugged display costs that time
// and nothing else.
void startDisplay() {
  startDisplayBus();
  displayPanel.initR(INITR_144GREENTAB);
  displayPanel.setRotation(DISPLAY_ROTATION);
  displayPanel.fillScreen(ST77XX_BLACK);
  bool imageMissing = (displayImage.getBuffer() == nullptr);
  if (imageMissing) {
    logMessage("ERROR no memory for the display image, screen stays blank");
  }
  shownState = readDisplayState(shownSensorSamples);
  drawScreen(shownState);
  analogWriteFreq(BACKLIGHT_PWM_FREQUENCY_HZ);
  analogWriteRange(BACKLIGHT_PWM_RANGE);
  writeBacklightLevel(backlightLevel);
}
