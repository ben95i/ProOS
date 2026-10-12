// =====================================================================================
// WindowLamp.ino
// Raspberry Pi Pico W (arduino-pico core) living room window lamp controller:
//   - lamp #1: a switched lamp, toggled by the power button
//   - lamp #2: one 12-bit PWM LED with anti-logarithmic brightness and non-blocking fades,
//     dimmed by the rotary encoder
//   - HTTP command API on port 4000
//   - rolling in-memory log returned over HTTP
//
// The web server, command decoder, PWM output and log are taken from BathroomLight.ino.
//
// Layout: every type and global is declared first, then every function is defined
// before the functions that call it, so the sketch needs no function prototypes.
// Keeping all types above the first function also keeps the Arduino IDE's own
// automatic prototypes valid.
// =====================================================================================

// shared libraries
#include <WiFi.h>
#include <neotimer.h>
#include <RotaryEncoder.h>
#include <stdarg.h>

// local libraries
#include "creds.h"

// ---- GPIO pins ----
const uint8_t POWER_BUTTON_PIN = 3;
const uint8_t LAMP1_POWER_PIN = 21;
const uint8_t LAMP2_POWER_PIN = 4;
const uint8_t ENCODER_PIN_A = 14;
const uint8_t ENCODER_PIN_B = 15;
const uint8_t CYW43_GPIO_PIN_OFFSET = 64; // arduino-pico numbers the Wi-Fi chip's GPIOs from 64
const uint8_t REGULATOR_MODE_PIN = CYW43_GPIO_PIN_OFFSET + CYW43_WL_GPIO_SMPS_PIN; // RT6150 PS pin, HIGH = PWM mode

// ---- common resources ----
const uint32_t SERIAL_BAUD_RATE = 115200;
const char NULL_TERMINATOR = '\0';
const char NEW_LINE = '\n';
const char CARRIAGE_RETURN = '\r';
const uint32_t MS_PER_SECOND = 1000;
const uint32_t SECONDS_PER_MINUTE = 60;
const uint32_t SECONDS_PER_HOUR = 3600;
const uint32_t SECONDS_PER_DAY = 86400;
const uint32_t MINUTES_PER_HOUR = 60;
const uint32_t HOURS_PER_DAY = 24;
const uint16_t STARTUP_SETTLE_MS = 1000; // pause after starting Wi-Fi, before the web server starts

// ---- Wi-Fi and web server ----
const uint16_t HTTP_SERVER_PORT = 4000;
const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000; // how often a dropped connection is retried
const uint32_t WIFI_STATUS_INTERVAL_MS = 1000;     // how often connection changes are checked for the log
const uint16_t WIFI_DISCONNECT_SETTLE_MS = 500;    // pause between disconnecting and reconnecting
const uint32_t WIFI_POWER_SAVE_BITS = 0xf;         // power-save mode bits cleared to disable power management
WiFiServer httpServer(HTTP_SERVER_PORT);
Neotimer wifiReconnectTimer = Neotimer(WIFI_RECONNECT_INTERVAL_MS);
Neotimer wifiStatusTimer = Neotimer(WIFI_STATUS_INTERVAL_MS);
bool wifiConnected = false; // last connection state written to the log

// ---- rolling log (the oldest entries are overwritten, each is stamped with the uptime) ----
const uint8_t LOG_CAPACITY = 60;     // entries kept
const uint8_t LOG_TEXT_LENGTH = 80;  // max characters per entry, including the terminator
const uint8_t TIMESTAMP_TEXT_SIZE = 24; // "+<days>d HH:MM:SS" plus terminator
struct LogEntry {
  uint32_t uptimeMs;             // millis() when logged
  char text[LOG_TEXT_LENGTH];
};
LogEntry logBuffer[LOG_CAPACITY];
uint8_t logNextIndex = 0;  // entry that the next message overwrites
uint8_t logEntryCount = 0; // entries stored so far, up to LOG_CAPACITY

// ---- command decoder: requests carry "?<command>,<unused>,<value>~" in the URL ----
const uint8_t COMMAND_MESSAGE_LENGTH = 32;
const uint8_t COMMAND_FIELD_MAX_DIGITS = 4;
const char COMMAND_FRAME_START = '?';
const char COMMAND_FRAME_END = '~';
const char COMMAND_FIELD_DELIMITER = ',';
const uint8_t COMMAND_FIRST_FIELD_START = 0;
const uint8_t COMMAND_VALUE_DELIMITER_OCCURRENCE = 2; // the value follows the second delimiter
const uint8_t COMMAND_DELIMITER_NOT_FOUND = 0;        // the second delimiter can never be at position 0
char commandMessageIn[COMMAND_MESSAGE_LENGTH + 1];
char commandReplyOut[COMMAND_MESSAGE_LENGTH + 1];
uint16_t commandFramePosition = 0;
bool commandFrameReading = false;

// command numbers (first field)
const uint16_t COMMAND_LAMP1 = 1;         // value: lamp #1 action below
const uint16_t COMMAND_LAMP2 = 2;         // value: lamp #2 action below
const uint16_t COMMAND_SET_LAMP2 = 3;     // value: level 0-255
const uint16_t COMMAND_READ_ALL = 4;      // replies "<lamp #1 state>~<lamp #2 level>"
const uint16_t COMMAND_READ_LOG = 9;      // value: 0 = all entries, N = newest N

// lamp #1 actions (value of COMMAND_LAMP1), each replies 1 = on, 0 = off
const uint16_t LAMP1_ACTION_READ_STATE = 0;
const uint16_t LAMP1_ACTION_TOGGLE = 1;
const uint16_t LAMP1_ACTION_OFF = 2;
const uint16_t LAMP1_ACTION_ON = 3;

// lamp #2 actions (value of COMMAND_LAMP2), each replies with a level 0-255
const uint16_t LAMP2_ACTION_FADE_OFF = 0;
const uint16_t LAMP2_ACTION_FADE_ON = 1;
const uint16_t LAMP2_ACTION_READ_LEVEL = 2;

// ---- HTTP buffers ----
const uint8_t HTTP_REQUEST_BUFFER_SIZE = 255;
const uint16_t HTTP_CLIENT_TIMEOUT_MS = 1000; // client inactivity timeout
const uint16_t HTTP_BODY_BUFFER_SIZE = 6144;  // multi-line replies (log)
const uint8_t HTTP_LINE_ENDING_LENGTH = 1;    // the newline sent after every reply body
char httpRequestBuffer[HTTP_REQUEST_BUFFER_SIZE];
char httpBodyBuffer[HTTP_BODY_BUFFER_SIZE];
uint16_t httpBodyLength = 0;
bool httpReplyUsesBody = false; // true when the reply is in httpBodyBuffer instead of commandReplyOut

// ---- power button ----
const uint8_t BUTTON_DEBOUNCE_MS = 75;
const uint8_t BUTTON_PRESSED = LOW;      // the button pulls its pin to ground
const uint8_t BUTTON_RELEASED = HIGH;
uint8_t buttonLastReading = BUTTON_RELEASED; // raw pin state on the previous pass
uint8_t buttonState = BUTTON_RELEASED;       // debounced state
uint32_t buttonLastChangeMs = 0;             // when the raw pin state last changed

// ---- rotary encoder ----
// LatchMode::FOUR3 reports one position count per physical detent
RotaryEncoder rotaryEncoder(ENCODER_PIN_B, ENCODER_PIN_A, RotaryEncoder::LatchMode::FOUR3);
const long ENCODER_LEVELS_PER_DETENT = 8; // brightness levels applied per detent
long encoderLastPosition = 0;             // position already applied to the brightness

// ---- lamp #1 (switched) ----
const uint8_t LAMP1_PIN_ON = LOW;   // the output is active low
const uint8_t LAMP1_PIN_OFF = HIGH;
const int16_t LAMP_STATE_OFF = 0;   // reply values
const int16_t LAMP_STATE_ON = 1;
bool lamp1On = false;

// ---- lamp #2 (PWM LED) ----
const uint8_t PWM_MAX_LEVEL = 255;               // API brightness range: 0 = off, 255 = 100%
const uint16_t PWM_DUTY_MAX = 4095;              // 12-bit duty cycle: 0 = off, 4095 = always on
// The PWM counter runs from the system clock through a whole-number divider, so every period is
// exactly the same length (a fractional divider is dithered, which shimmers at low brightness).
// At 125MHz: 1 = 30.5kHz, 2 = 15.3kHz, 4 = 7.6kHz. 2 is the divider BathroomLight uses. It is
// 12x above the 1.28kHz resonance of this lamp's LC filter; the earlier 22kHz was 17x above.
const uint32_t PWM_CLOCK_DIVIDER = 2;
static_assert(PWM_CLOCK_DIVIDER >= 1 && PWM_CLOCK_DIVIDER <= 255, "the PWM clock divider must be 1-255");
const uint32_t PWM_CLOCKS_PER_PERIOD = PWM_DUTY_MAX * PWM_CLOCK_DIVIDER; // system clocks in one PWM period
const uint32_t PWM_FREQUENCY_HZ = F_CPU / PWM_CLOCKS_PER_PERIOD;
const uint8_t LAMP2_FADE_STEP_MS = 8;            // per level, ~2s full range
const uint16_t ACTIVITY_LED_BLINK_MS = 100;      // on-board LED lights this long for each command
bool activityLedOn = false;                      // true while the on-board LED shows a command
uint32_t activityLedOnMs = 0;                    // when the on-board LED was last lit
static_assert(LAMP2_FADE_STEP_MS > 0, "fade step time must not be zero");
int16_t lamp2Level = 0;                          // 0 = off, 255 = 100%
int16_t lamp2TargetLevel = 0;                    // level the LED is at or heading to, a fade runs while they differ
uint32_t lamp2LastStepMs = 0;                    // time of the last fade step
// CIE 1931 lightness curve: converts a 0-255 brightness level to a 12-bit duty cycle so equal
// level steps look like equal brightness steps. Every level has its own duty cycle, so level 1
// is just visible and fades have no flat spots.
const uint16_t ANTI_LOGARITHMIC_TABLE[256] = {
      0,    2,    4,    5,    7,    9,   11,   12,   14,   16,   18,   20,   21,   23,   25,   27,
     28,   30,   32,   34,   36,   37,   39,   41,   43,   45,   47,   49,   52,   54,   56,   59,
     61,   64,   66,   69,   72,   75,   77,   80,   83,   87,   90,   93,   96,  100,  103,  107,
    111,  115,  118,  122,  126,  131,  135,  139,  144,  148,  153,  157,  162,  167,  172,  177,
    182,  187,  193,  198,  204,  209,  215,  221,  227,  233,  239,  246,  252,  259,  265,  272,
    279,  286,  293,  300,  308,  315,  323,  330,  338,  346,  354,  362,  371,  379,  388,  396,
    405,  414,  423,  432,  442,  451,  461,  470,  480,  490,  501,  511,  521,  532,  543,  553,
    564,  576,  587,  598,  610,  622,  634,  646,  658,  670,  683,  695,  708,  721,  734,  748,
    761,  775,  788,  802,  816,  831,  845,  860,  874,  889,  904,  920,  935,  951,  966,  982,
    999, 1015, 1031, 1048, 1065, 1082, 1099, 1116, 1134, 1152, 1170, 1188, 1206, 1224, 1243, 1262,
   1281, 1300, 1320, 1339, 1359, 1379, 1399, 1420, 1440, 1461, 1482, 1503, 1525, 1546, 1568, 1590,
   1612, 1635, 1657, 1680, 1703, 1726, 1750, 1774, 1797, 1822, 1846, 1870, 1895, 1920, 1945, 1971,
   1996, 2022, 2048, 2074, 2101, 2128, 2155, 2182, 2209, 2237, 2265, 2293, 2321, 2350, 2378, 2407,
   2437, 2466, 2496, 2526, 2556, 2587, 2617, 2648, 2679, 2711, 2743, 2774, 2807, 2839, 2872, 2905,
   2938, 2971, 3005, 3039, 3073, 3107, 3142, 3177, 3212, 3248, 3283, 3319, 3356, 3392, 3429, 3466,
   3503, 3541, 3578, 3617, 3655, 3694, 3732, 3772, 3811, 3851, 3891, 3931, 3972, 4012, 4054, 4095,
};

// ---- brightness change log ----
// Turning the encoder or dragging the website's slider sets many levels in a row. They are
// logged as one entry ("encoder: backlight 40 -> 120") once the changes have stopped for
// LEVEL_LOG_QUIET_MS, so a long turn doesn't push everything else out of the log.
const uint16_t LEVEL_LOG_QUIET_MS = 1000;
enum LevelChangeSource {
  LEVEL_SOURCE_NONE,
  LEVEL_SOURCE_ENCODER,
  LEVEL_SOURCE_HTTP,
  LEVEL_SOURCE_COUNT
};
// the name each source is logged under, in LevelChangeSource order
const char *const LEVEL_SOURCE_NAMES[] = { "none", "encoder", "http" };
static_assert(sizeof(LEVEL_SOURCE_NAMES) / sizeof(LEVEL_SOURCE_NAMES[0]) == LEVEL_SOURCE_COUNT, "one name per LevelChangeSource");
LevelChangeSource pendingLevelSource = LEVEL_SOURCE_NONE; // source of the changes not yet logged
int16_t pendingLevelStart = 0;                            // level before the first of those changes
uint32_t pendingLevelLastMs = 0;                          // time of the latest of those changes

// ==== rolling log ====

// Adds one printf-style entry to the rolling log, overwriting the oldest entry when full.
// Each entry records the uptime it was logged at, and is also printed on the USB serial port.
void logMessage(const char *format, ...) {
  LogEntry &entry = logBuffer[logNextIndex];
  entry.uptimeMs = millis();
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.text, sizeof(entry.text), format, arguments);
  va_end(arguments);
  Serial.println(entry.text);
  uint16_t followingIndex = logNextIndex + 1;
  logNextIndex = followingIndex % LOG_CAPACITY;
  if (logEntryCount < LOG_CAPACITY) {
    logEntryCount = logEntryCount + 1;
  }
}

// Formats an uptime as "+<days>d HH:MM:SS".
void formatUptime(char *output, size_t outputSize, uint32_t uptimeMs) {
  uint32_t totalSeconds = uptimeMs / MS_PER_SECOND;
  uint32_t totalMinutes = totalSeconds / SECONDS_PER_MINUTE;
  uint32_t totalHours = totalSeconds / SECONDS_PER_HOUR;
  uint32_t days = totalSeconds / SECONDS_PER_DAY;
  uint32_t hours = totalHours % HOURS_PER_DAY;
  uint32_t minutes = totalMinutes % MINUTES_PER_HOUR;
  uint32_t seconds = totalSeconds % SECONDS_PER_MINUTE;
  snprintf(output, outputSize, "+%lud %02lu:%02lu:%02lu",
           (unsigned long)days, (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
}

// ==== HTTP reply text ====

// Starts a multi-line reply in httpBodyBuffer, replacing the single-value reply.
void startBodyReply() {
  httpBodyLength = 0;
  httpBodyBuffer[0] = NULL_TERMINATOR;
  httpReplyUsesBody = true;
}

// Appends printf-style text to the multi-line reply; text that does not fit is cut off. The
// length is summed in 32 bits so it can never wrap around.
void appendBody(const char *format, ...) {
  uint16_t lastUsableIndex = HTTP_BODY_BUFFER_SIZE - 1;
  if (httpBodyLength >= lastUsableIndex) {
    return;
  }
  char *writePosition = httpBodyBuffer + httpBodyLength;
  size_t spaceRemaining = HTTP_BODY_BUFFER_SIZE - httpBodyLength;
  va_list arguments;
  va_start(arguments, format);
  int charactersWritten = vsnprintf(writePosition, spaceRemaining, format, arguments);
  va_end(arguments);
  if (charactersWritten > 0) {
    uint32_t extendedLength = (uint32_t)httpBodyLength + (uint32_t)charactersWritten;
    if (extendedLength > lastUsableIndex) {
      extendedLength = lastUsableIndex;
    }
    httpBodyLength = (uint16_t)extendedLength;
  }
}

// Sets a single number as the reply.
void replyWithNumber(int16_t value) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%d", value);
}

// Replies with the newest log entries, oldest first, one per line (0 = every stored entry).
void writeLog(uint16_t requestedEntries) {
  startBodyReply();
  uint8_t entriesToShow = logEntryCount;
  bool limitRequested = (requestedEntries != 0);
  bool limitWithinStored = (requestedEntries <= logEntryCount);
  if (limitRequested && limitWithinStored) {
    entriesToShow = (uint8_t)requestedEntries;
  }
  if (entriesToShow == 0) {
    appendBody("(log empty)");
    return;
  }
  // the oldest entry shown sits entriesToShow places behind the next write position
  uint16_t oldestShownOffset = LOG_CAPACITY - entriesToShow;
  for (uint8_t position = 0; position < entriesToShow; position++) {
    uint16_t unwrappedIndex = logNextIndex + oldestShownOffset + position;
    uint16_t bufferIndex = unwrappedIndex % LOG_CAPACITY;
    const LogEntry &entry = logBuffer[bufferIndex];
    char timestamp[TIMESTAMP_TEXT_SIZE];
    formatUptime(timestamp, sizeof(timestamp), entry.uptimeMs);
    uint16_t entryNumber = position + 1;
    bool isLastEntry = (entryNumber == entriesToShow);
    const char *lineEnding = "\n";
    if (isLastEntry) {
      lineEnding = "";
    }
    appendBody("%s  %s%s", timestamp, entry.text, lineEnding);
  }
}

// ==== lamp #1 ====

// Returns lamp #1's state as a reply value.
int16_t lamp1ReplyState() {
  if (lamp1On) {
    return LAMP_STATE_ON;
  }
  return LAMP_STATE_OFF;
}

// Switches lamp #1 and logs it under the source that asked ("button" or "http"). A lamp that
// is already in the requested state is left alone and nothing is logged, so the repeated on and
// off commands of the room scenes don't fill the log.
void setLamp1(bool turnOn, const char *source) {
  bool alreadyThere = (turnOn == lamp1On);
  if (alreadyThere) {
    return;
  }
  lamp1On = turnOn;
  if (lamp1On) {
    digitalWrite(LAMP1_POWER_PIN, LAMP1_PIN_ON);
    logMessage("%s: lamp on", source);
  } else {
    digitalWrite(LAMP1_POWER_PIN, LAMP1_PIN_OFF);
    logMessage("%s: lamp off", source);
  }
}

// ==== lamp #2 ====

// Holds the Pico W's RT6150 3.3V regulator in fixed-frequency PWM mode. Its default
// power-saving mode skips switching cycles at light load, which leaves a low-frequency
// ripple on the 3.3V rail; PWM mode keeps the rail steady for a few mA more idle current.
void forceRegulatorPwmMode() {
  pinMode(REGULATOR_MODE_PIN, OUTPUT);
  digitalWrite(REGULATOR_MODE_PIN, HIGH);
}

// Limits a requested brightness level to 0-255.
int16_t clampPwmLevel(int16_t requestedLevel) {
  int16_t clampedLevel = requestedLevel;
  if (clampedLevel >= PWM_MAX_LEVEL) {
    clampedLevel = PWM_MAX_LEVEL;
  }
  if (clampedLevel <= 0) {
    clampedLevel = 0;
  }
  return clampedLevel;
}

// Sets lamp #2's brightness: the level is clamped, converted to a 12-bit duty cycle through
// the anti-logarithmic table, and fully on or off levels use a plain digital output.
void setLamp2(int16_t requestedLevel) {
  int16_t level = clampPwmLevel(requestedLevel);
  uint16_t dutyCycle = ANTI_LOGARITHMIC_TABLE[level];
  bool fullyOn = (dutyCycle == PWM_DUTY_MAX);
  bool fullyOff = (dutyCycle == 0);
  if (fullyOn) {
    digitalWrite(LAMP2_POWER_PIN, HIGH);
  } else if (fullyOff) {
    digitalWrite(LAMP2_POWER_PIN, LOW);
  } else {
    analogWrite(LAMP2_POWER_PIN, dutyCycle);
  }
  lamp2Level = level;
}

// Starts a non-blocking fade from the current level to the target level. A new fade replaces
// any fade already in progress.
void startFadeLamp2(int16_t requestedTarget) {
  lamp2TargetLevel = clampPwmLevel(requestedTarget);
  lamp2LastStepMs = millis();
}

// Advances an active fade (the level differs from the target); called from the superloop. Steps
// missed while the loop was busy (e.g. serving a slow client) are applied together, so the fade
// keeps its overall duration.
void updateLamp2Fade() {
  bool fadeInProgress = (lamp2Level != lamp2TargetLevel);
  if (!fadeInProgress) {
    return;
  }
  uint32_t msSinceLastStep = millis() - lamp2LastStepMs;
  bool stepIsDue = (msSinceLastStep >= LAMP2_FADE_STEP_MS);
  if (!stepIsDue) {
    return;
  }
  uint32_t stepsDue = msSinceLastStep / LAMP2_FADE_STEP_MS;
  uint32_t msAccountedFor = stepsDue * LAMP2_FADE_STEP_MS;
  lamp2LastStepMs = lamp2LastStepMs + msAccountedFor;
  int16_t levelDifference = lamp2TargetLevel - lamp2Level;
  int16_t stepsRemaining = abs(levelDifference);
  if (stepsDue > (uint32_t)stepsRemaining) {
    stepsDue = stepsRemaining;
  }
  int16_t stepsToApply = (int16_t)stepsDue;
  bool fadingUp = (lamp2TargetLevel > lamp2Level);
  int16_t nextLevel = 0;
  if (fadingUp) {
    nextLevel = lamp2Level + stepsToApply;
  } else {
    nextLevel = lamp2Level - stepsToApply;
  }
  setLamp2(nextLevel);
}

// Lights the on-board LED to show a command ran; updateActivityLed() turns it off again, so
// nothing waits. A command arriving while it is lit restarts the blink.
void blinkActivityLed() {
  if (!activityLedOn) {
    digitalWrite(LED_BUILTIN, HIGH);
  }
  activityLedOn = true;
  activityLedOnMs = millis();
}

// Turns the on-board LED off once it has been lit for ACTIVITY_LED_BLINK_MS; called from the superloop.
void updateActivityLed() {
  if (!activityLedOn) {
    return;
  }
  uint32_t msLit = millis() - activityLedOnMs;
  bool blinkFinished = (msLit >= ACTIVITY_LED_BLINK_MS);
  if (!blinkFinished) {
    return;
  }
  digitalWrite(LED_BUILTIN, LOW);
  activityLedOn = false;
}

// ==== brightness change log ====

// Logs the brightness changes collected so far as one entry, if there are any.
void flushLevelLog() {
  bool nothingPending = (pendingLevelSource == LEVEL_SOURCE_NONE);
  if (nothingPending) {
    return;
  }
  const char *sourceName = LEVEL_SOURCE_NAMES[pendingLevelSource];
  logMessage("%s: backlight %d -> %d", sourceName, pendingLevelStart, lamp2Level);
  pendingLevelSource = LEVEL_SOURCE_NONE;
}

// Logs the collected brightness changes once they have stopped for LEVEL_LOG_QUIET_MS; called
// from the superloop.
void updateLevelLog() {
  bool nothingPending = (pendingLevelSource == LEVEL_SOURCE_NONE);
  if (nothingPending) {
    return;
  }
  uint32_t msSinceLastChange = millis() - pendingLevelLastMs;
  bool changesHaveStopped = (msSinceLastChange >= LEVEL_LOG_QUIET_MS);
  if (changesHaveStopped) {
    flushLevelLog();
  }
}

// Jumps lamp #2 straight to a level, cancelling any fade, and collects the change for the log.
// Changes from a different source than the ones already collected start a new entry.
void setLamp2Directly(int16_t requestedLevel, LevelChangeSource source) {
  bool sourceChanged = (source != pendingLevelSource);
  if (sourceChanged) {
    flushLevelLog();
    pendingLevelSource = source;
    pendingLevelStart = lamp2Level;
  }
  pendingLevelLastMs = millis();
  setLamp2(requestedLevel);
  lamp2TargetLevel = lamp2Level;
}

// ==== power button and encoder ====

// Interrupt service routine: updates the encoder state machine on any pin edge.
void encoderISR() {
  rotaryEncoder.tick();
}

// Reads the debounced power button; each press toggles lamp #1.
void readPowerButton() {
  uint8_t reading = digitalRead(POWER_BUTTON_PIN);
  bool readingChanged = (reading != buttonLastReading);
  if (readingChanged) {
    // restart the debounce time
    buttonLastChangeMs = millis();
  }
  buttonLastReading = reading;
  uint32_t msSinceChange = millis() - buttonLastChangeMs;
  bool readingIsSteady = (msSinceChange > BUTTON_DEBOUNCE_MS);
  if (!readingIsSteady) {
    return;
  }
  bool stateChanged = (reading != buttonState);
  if (!stateChanged) {
    return;
  }
  buttonState = reading;
  bool pressed = (buttonState == BUTTON_PRESSED);
  if (pressed) {
    setLamp1(!lamp1On, "button");
  }
}

// Applies the encoder movement since the last pass to lamp #2's brightness. A positive
// position change (encoder turned left) lowers the brightness.
void readEncoder() {
  long encoderPosition = rotaryEncoder.getPosition();
  long positionChange = encoderPosition - encoderLastPosition;
  bool encoderMoved = (positionChange != 0);
  if (!encoderMoved) {
    return;
  }
  encoderLastPosition = encoderPosition;
  // worked out and limited as a long, so a large movement can't overflow the 16-bit level
  long levelChange = positionChange * ENCODER_LEVELS_PER_DETENT;
  long requestedLevel = (long)lamp2Level - levelChange;
  if (requestedLevel > PWM_MAX_LEVEL) {
    requestedLevel = PWM_MAX_LEVEL;
  }
  if (requestedLevel < 0) {
    requestedLevel = 0;
  }
  setLamp2Directly((int16_t)requestedLevel, LEVEL_SOURCE_ENCODER);
}

// ==== Wi-Fi ====

// Sets Wi-Fi client mode with the hostname from creds.h and connects to the configured network.
void connectWiFi() {
  logMessage("connecting to Wi-Fi %s", ssid);
  WiFi.mode(WIFI_STA);
  // the hostname must be set before WiFi.begin so the DHCP request carries it
  WiFi.setHostname(hostname);
  // disable power management by clearing the power-save mode bits
  uint32_t powerManagementMode = CYW43_DEFAULT_PM & ~WIFI_POWER_SAVE_BITS;
  cyw43_wifi_pm(&cyw43_state, powerManagementMode);
  WiFi.begin(ssid, ssid_pwd);
}

// Logs when the Wi-Fi connection is made or lost, checked once a second.
void logWiFiStateChanges() {
  bool checkIsDue = wifiStatusTimer.repeat();
  if (!checkIsDue) {
    return;
  }
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  bool stateChanged = (connectedNow != wifiConnected);
  if (!stateChanged) {
    return;
  }
  wifiConnected = connectedNow;
  if (connectedNow) {
    IPAddress localAddress = WiFi.localIP();
    String addressText = localAddress.toString(); // kept alive while its text is logged
    long signalStrength = (long)WiFi.RSSI();
    logMessage("Wi-Fi connected, IP %s, RSSI %ld dBm", addressText.c_str(), signalStrength);
  } else {
    logMessage("Wi-Fi connection lost");
  }
}

// Reconnects a dropped Wi-Fi connection, checked every WIFI_RECONNECT_INTERVAL_MS.
void reconnectWiFiWhenDropped() {
  bool checkIsDue = wifiReconnectTimer.repeat();
  if (!checkIsDue) {
    return;
  }
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  if (connectedNow) {
    return;
  }
  WiFi.disconnect();
  delay(WIFI_DISCONNECT_SETTLE_MS);
  connectWiFi();
}

// ==== commands ====

// Lamp #1 controls (command 1): read the state, toggle, off, on. Each replies with the state
// the lamp ends in.
void runLamp1Command(uint16_t action) {
  switch (action) {
    case LAMP1_ACTION_READ_STATE:
      break;
    case LAMP1_ACTION_TOGGLE:
      setLamp1(!lamp1On, "http");
      break;
    case LAMP1_ACTION_OFF:
      setLamp1(false, "http");
      break;
    case LAMP1_ACTION_ON:
      setLamp1(true, "http");
      break;
    default:
      replyWithNumber(0);
      return;
  }
  replyWithNumber(lamp1ReplyState());
}

// Fades lamp #2 to a level by command and replies with that level. The fade runs from the
// superloop, so the reply doesn't wait for it. Nothing is logged when the lamp is already at
// the level or fading to it.
void fadeLamp2ByCommand(int16_t targetLevel, const char *description) {
  flushLevelLog(); // keeps the log in the order things happened
  bool alreadyHeadingThere = (lamp2TargetLevel == targetLevel);
  if (!alreadyHeadingThere) {
    startFadeLamp2(targetLevel);
    logMessage("http: backlight %s", description);
  }
  replyWithNumber(lamp2TargetLevel);
}

// Lamp #2 controls (command 2): fade off, fade on, read the level. The level read is the one
// the lamp is at or fading to.
void runLamp2Command(uint16_t action) {
  switch (action) {
    case LAMP2_ACTION_FADE_OFF:
      fadeLamp2ByCommand(0, "fading off");
      break;
    case LAMP2_ACTION_FADE_ON:
      fadeLamp2ByCommand(PWM_MAX_LEVEL, "fading on");
      break;
    case LAMP2_ACTION_READ_LEVEL:
      replyWithNumber(lamp2TargetLevel);
      break;
    default:
      replyWithNumber(0);
      break;
  }
}

// Direct level control (command 3): lamp #2 jumps to the level, cancelling any fade.
void setLamp2ByCommand(uint16_t requestedLevel) {
  int16_t level = PWM_MAX_LEVEL;
  bool withinRange = (requestedLevel < PWM_MAX_LEVEL);
  if (withinRange) {
    level = (int16_t)requestedLevel;
  }
  setLamp2Directly(level, LEVEL_SOURCE_HTTP);
  replyWithNumber(lamp2Level);
}

// Replies with both lamps (command 4) as "<lamp #1 state>~<lamp #2 level>".
void replyWithAllLamps() {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%d~%d", lamp1ReplyState(), lamp2TargetLevel);
}

// Runs a decoded command and blinks the on-board LED. The command numbers are kept from earlier versions so existing
// clients keep working.
void runCommand(uint16_t command, uint16_t value) {
  blinkActivityLed();
  switch (command) {
    case COMMAND_LAMP1:
      runLamp1Command(value);
      break;
    case COMMAND_LAMP2:
      runLamp2Command(value);
      break;
    case COMMAND_SET_LAMP2:
      setLamp2ByCommand(value);
      break;
    case COMMAND_READ_ALL:
      replyWithAllLamps();
      break;
    case COMMAND_READ_LOG:
      writeLog(value);
      break;
    default:
      replyWithNumber(0);
      break;
  }
}

// Returns the position of the given delimiter occurrence (1 = first) in the command message,
// or COMMAND_DELIMITER_NOT_FOUND when the message has fewer delimiters.
uint8_t findCommandDelimiter(uint8_t occurrence) {
  uint8_t delimitersSeen = 0;
  for (uint8_t position = 0; position < COMMAND_MESSAGE_LENGTH; position++) {
    char character = commandMessageIn[position];
    if (character == NULL_TERMINATOR) {
      break;
    }
    if (character == COMMAND_FIELD_DELIMITER) {
      delimitersSeen = delimitersSeen + 1;
      if (delimitersSeen == occurrence) {
        return position;
      }
    }
  }
  return COMMAND_DELIMITER_NOT_FOUND;
}

// Parses the number in the field starting at startPosition. The field ends at a delimiter,
// the end of the message or after COMMAND_FIELD_MAX_DIGITS characters. A negative number
// counts as 0.
uint16_t parseCommandField(uint8_t startPosition) {
  char fieldText[COMMAND_FIELD_MAX_DIGITS + 1];
  uint8_t fieldLength = 0;
  uint8_t readPosition = startPosition;
  while (fieldLength < COMMAND_FIELD_MAX_DIGITS) {
    // bounds guard: the message is always terminated before its buffer ends
    bool insideMessage = (readPosition < COMMAND_MESSAGE_LENGTH);
    if (!insideMessage) {
      break;
    }
    char character = commandMessageIn[readPosition];
    bool reachedDelimiter = (character == COMMAND_FIELD_DELIMITER);
    bool reachedEnd = (character == NULL_TERMINATOR);
    if (reachedDelimiter || reachedEnd) {
      break;
    }
    fieldText[fieldLength] = character;
    fieldLength = fieldLength + 1;
    readPosition = readPosition + 1;
  }
  fieldText[fieldLength] = NULL_TERMINATOR;
  int parsedNumber = atoi(fieldText);
  if (parsedNumber < 0) {
    parsedNumber = 0;
  }
  uint16_t fieldValue = (uint16_t)parsedNumber;
  return fieldValue;
}

// Decodes "<command>,<unused>,<value>" and runs it; messages with fewer than two delimiters
// are logged and ignored.
void decodeCommandMessage() {
  uint8_t valueDelimiterPosition = findCommandDelimiter(COMMAND_VALUE_DELIMITER_OCCURRENCE);
  bool enoughDelimiters = (valueDelimiterPosition != COMMAND_DELIMITER_NOT_FOUND);
  if (!enoughDelimiters) {
    logMessage("invalid command frame: %s", commandMessageIn);
    return;
  }
  uint16_t command = parseCommandField(COMMAND_FIRST_FIELD_START);
  uint8_t valueStartPosition = valueDelimiterPosition + 1;
  uint16_t value = parseCommandField(valueStartPosition);
  runCommand(command, value);
}

// Collects the characters between '?' and '~' and decodes the message at the closing '~'.
// Characters beyond the message buffer are dropped.
void processCommandCharacter(char receivedCharacter) {
  if (!commandFrameReading) {
    bool frameStarts = (receivedCharacter == COMMAND_FRAME_START);
    if (frameStarts) {
      commandFrameReading = true;
      commandFramePosition = 0;
    }
    return;
  }
  bool frameEnds = (receivedCharacter == COMMAND_FRAME_END);
  if (frameEnds) {
    commandMessageIn[commandFramePosition] = NULL_TERMINATOR;
    commandFrameReading = false;
    decodeCommandMessage();
    return;
  }
  bool isFrameStart = (receivedCharacter == COMMAND_FRAME_START);
  uint16_t lastStorablePosition = COMMAND_MESSAGE_LENGTH - 1;
  bool hasSpace = (commandFramePosition < lastStorablePosition);
  if (!isFrameStart && hasSpace) {
    commandMessageIn[commandFramePosition] = receivedCharacter;
    commandFramePosition = commandFramePosition + 1;
  }
}

// ==== HTTP server ====

// Writes a complete HTTP response with a plain-text body followed by a newline.
void sendHttpResponse(WiFiClient &client, const char *status, const char *body) {
  size_t bodyLength = strlen(body);
  size_t contentLength = bodyLength + HTTP_LINE_ENDING_LENGTH;
  client.print("HTTP/1.1 ");
  client.println(status);
  client.println("Content-Type: text/plain");
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println("Connection: close");
  client.println();
  client.print(body);
  client.print(NEW_LINE);
  client.flush(); // wait for the response to leave before the connection closes
}

// Decodes the command in the request line and replies. Every request starts with a clean
// decoder and an empty reply, so a half-received frame or an old result never leaks into it.
// Only the request line is parsed: headers such as Referer may also contain '?'.
void handleHttpRequest(WiFiClient &client, uint8_t requestLength) {
  commandFrameReading = false;
  commandReplyOut[0] = NULL_TERMINATOR;
  httpReplyUsesBody = false;
  for (uint8_t position = 0; position < requestLength; position++) {
    char character = httpRequestBuffer[position];
    bool endOfRequestLine = (character == CARRIAGE_RETURN) || (character == NEW_LINE) || (character == NULL_TERMINATOR);
    if (endOfRequestLine) {
      break;
    }
    processCommandCharacter(character);
  }
  bool noValidCommand = (commandReplyOut[0] == NULL_TERMINATOR);
  if (httpReplyUsesBody) {
    sendHttpResponse(client, "200 OK", httpBodyBuffer);
  } else if (noValidCommand) {
    sendHttpResponse(client, "400 Bad Request", "ERR");
  } else {
    sendHttpResponse(client, "200 OK", commandReplyOut);
  }
}

// Reads the request headers into httpRequestBuffer until the blank line that ends them;
// characters beyond the buffer are dropped. Returns true when a complete request arrived,
// false when the client disconnected or stalled for HTTP_CLIENT_TIMEOUT_MS.
bool readHttpRequest(WiFiClient &client, uint8_t &requestLength) {
  requestLength = 0;
  bool lineIsBlank = true;
  uint32_t lastActivityMs = millis();
  while (client.connected()) {
    uint32_t msSinceActivity = millis() - lastActivityMs;
    bool timedOut = (msSinceActivity > HTTP_CLIENT_TIMEOUT_MS);
    if (timedOut) {
      return false;
    }
    if (!client.available()) {
      continue;
    }
    int receivedValue = client.read();
    char receivedCharacter = (char)receivedValue;
    lastActivityMs = millis();
    uint8_t lastStorablePosition = HTTP_REQUEST_BUFFER_SIZE - 1;
    bool bufferHasSpace = (requestLength < lastStorablePosition);
    if (bufferHasSpace) {
      httpRequestBuffer[requestLength] = receivedCharacter;
      requestLength = requestLength + 1;
    }
    // the headers end with a blank line (\r\n followed by another \r\n)
    bool isNewLine = (receivedCharacter == NEW_LINE);
    if (isNewLine && lineIsBlank) {
      return true;
    }
    if (isNewLine) {
      lineIsBlank = true;
    } else if (receivedCharacter != CARRIAGE_RETURN) {
      lineIsBlank = false;
    }
  }
  return false;
}

// Serves one waiting HTTP client, if any: reads its request, replies, and closes it.
void serviceHttpClient() {
  WiFiClient client = httpServer.accept();
  if (!client) {
    return;
  }
  client.setNoDelay(true); // disable Nagle's algorithm
  uint8_t requestLength = 0;
  bool requestComplete = readHttpRequest(client, requestLength);
  if (requestComplete) {
    handleHttpRequest(client, requestLength);
  }
  client.stop();
}

// ==== main entry points ====

// Steadies the 3.3V regulator, sets up the lamp outputs, the button and the encoder, and starts
// networking. Both lamps start off.
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  forceRegulatorPwmMode();
  // initialise 12-bit PWM
  pinMode(LAMP2_POWER_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQUENCY_HZ);
  analogWriteRange(PWM_DUTY_MAX);
  digitalWrite(LAMP2_POWER_PIN, LOW);
  // the on-board LED stays lit while starting up
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LAMP1_POWER_PIN, OUTPUT);
  digitalWrite(LAMP1_POWER_PIN, LAMP1_PIN_OFF);
  // every RP2040 GPIO can raise an interrupt
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), encoderISR, CHANGE);
  logMessage("WindowLamp started");
  connectWiFi();
  delay(STARTUP_SETTLE_MS);
  httpServer.begin();
  digitalWrite(LED_BUILTIN, LOW);
}

// Superloop: every task is non-blocking.
void loop() {
  logWiFiStateChanges();
  reconnectWiFiWhenDropped();
  readPowerButton();
  readEncoder();
  updateLamp2Fade();
  updateLevelLog();
  updateActivityLed();
  serviceHttpClient();
}
