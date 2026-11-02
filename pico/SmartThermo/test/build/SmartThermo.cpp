#line 1 "SmartThermo.ino"
// =====================================================================================
// SmartThermo.ino
// Raspberry Pi Pico W (arduino-pico core) thermostat for a single-stage gas furnace with
// single-stage central AC on one 24 VAC transformer (terminals R, W1, Y1, G and C):
//   - three relays close R to W1 (furnace heat call), Y1 (compressor) and G (blower)
//   - hvac mode off / heat / cool, fan auto / on, setpoint 50-80 F in 1 F steps
//   - firmware interlocks from the HVAC Thermostat Control Reference: W1 and Y1 never closed
//     together, Y1 only with G, compressor minimum off and on times, a 5 minute delay after
//     power-up, a heat / cool changeover dead band, the furnace minimum on time, starts per
//     hour limits and a blower run-on after cooling
//   - every relay is open at boot and after a crash (the hardware watchdog restarts the Pico,
//     which resets the pins)
//   - DHT22 temperature and humidity sensor. No reading at startup locks the HVAC out (every
//     relay open); a sensor with no reading for 10 min runs heat at 25% duty, also after a restart,
//     and switches cool mode off. A reading must be possible indoors, and a jump of over 5 F must
//     repeat. Every sensor error is logged and reported in the status
//   - run limits: W1 and Y1 stop and rest 30 min after 1 h and 2 h with no temperature response,
//     or 3 h and 6 h in any one run. A reading that stays frozen through 3 such stops drops heat
//     to 25% duty and switches cool mode off
//   - safe mode after 3 watchdog restarts in a row: the HVAC and display run with no networking,
//     which is tried again after 1 h, so repeated stalls can't keep the HVAC in its power-up delay
//   - the HVAC keeps working without Wi-Fi: a join that fails after boot is retried every 15 s for
//     5 min; a lost connection is retried 10 times, 2 min apart, then the Pico restarts once no
//     heat or cool cycle is running
//   - HTTP command API on port 4000, settings stored in emulated EEPROM (one write per 5 min at most)
//   - rolling in-memory log and status returned over HTTP
//   - Adafruit 1.44" 128x128 ST7735R TFT on SPI1, showing the room temperature, the setpoint and
//     the fan, run and mode icons. Its backlight rests at about 70% by PWM and fades up to full for 30 s
//     when a setting is adjusted
//
// The two cores split the work so Wi-Fi can never hold up the HVAC: core 1 (setup1 / loop1)
// reads the sensor, runs the thermostat, drives the relays and feeds the watchdog; core 0
// (setup / loop) runs Wi-Fi, the HTTP server, the EEPROM saves and the display. Everything both
// cores touch (settings, thermostat state, relays, the log) is guarded by stateMutex, which is
// never held across a call to the Wi-Fi chip.
//
// Files:
//   SmartThermo.ino  the API routes, settings and EEPROM, Wi-Fi, the rolling log, the status
//                    reply, setup and both cores' loops
//   common.h         what the modules use from this file: constants, settings, helpers
//   http.cpp / .h    the HTTP server, the command decoder and the reply buffers
//   thermo.cpp / .h  the DHT22 sensor, the thermostat, the relays and their interlocks
//   wdog.cpp / .h    the hardware watchdog and the restart cause
//   display.cpp / .h the TFT display (JosefinSansDigits.h is its default font)
//
// Wi-Fi, the command decoder, the HTTP server, the rolling log and the settings storage are
// taken from BathroomLight.ino.
//
// Layout: every type and global is declared first, then every function is defined before the
// functions that call it, so this file needs no function prototypes of its own.
// =====================================================================================

// shared libraries
#include <WiFi.h>
#include <neotimer.h>
#include <EEPROM.h>
#include <stdarg.h>
#include <hardware/timer.h>
#include <pico/mutex.h>

// local libraries
#include "sim_creds.h"
#include "common.h"
#include "http.h"
#include "thermo.h"
#include "wdog.h"
#include "display.h"

// ---- cores and shared state ----
const uint16_t STARTUP_SETTLE_MS = 1000;  // pause after starting Wi-Fi, before the web server starts
const uint32_t CORE1_LOOP_PAUSE_MS = 10;  // pause between HVAC passes, outside the mutex
recursive_mutex_t stateMutex;       // guards everything both cores touch; recursive so logMessage() can nest
volatile bool controlReady = false; // set by setup() once the shared state is ready, core 1 waits for it
uint64_t bootMs = 0;                // uptime when setup() started

// ---- Wi-Fi ----
const uint32_t WIFI_STATUS_INTERVAL_MS = 1000;     // how often connection changes are checked for the log
const uint32_t WIFI_RETRY_INTERVAL_MS = 120000;    // 2 min between reconnect attempts
const uint32_t WIFI_STARTUP_RETRY_INTERVAL_MS = 15000; // gap between attempts until the first connection after boot
const uint64_t WIFI_STARTUP_WINDOW_MS = 300000;    // those quick attempts stop 5 min after boot
const uint8_t WIFI_RECONNECT_ATTEMPTS_MAX = 10;    // then the Pico restarts once heat and cool are off
const uint16_t WIFI_DISCONNECT_SETTLE_MS = 500;    // pause between disconnecting and reconnecting
const uint32_t WIFI_POWER_SAVE_BITS = 0xf;         // power-save mode bits cleared to disable power management
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;     // longest a (re)connect may hold up core 0
Neotimer wifiStatusTimer = Neotimer(WIFI_STATUS_INTERVAL_MS);
bool wifiConnected = false;          // last connection state written to the log
bool wifiHasConnected = false;       // true once Wi-Fi has connected since boot
bool wifiStartupRetryLogged = false; // the quick attempts after boot are logged once
uint64_t wifiLostSinceMs = 0;        // uptime when the connection was last lost (boot counts)
uint32_t wifiLastAttemptMs = 0;      // millis() of the last connection attempt
uint8_t wifiReconnectAttempts = 0;   // attempts since the connection was last up
bool wifiRestartPending = false;     // true once every attempt failed: restart when heat and cool are off
bool wifiRestartWaitLogged = false;  // the wait for the cycle to end is logged once
const uint8_t WIFI_STATUS_TEXT_SIZE = 64;
char wifiStatusText[WIFI_STATUS_TEXT_SIZE] = ""; // "connected, IP 192.168.1.22, RSSI -58 dBm", read outside stateMutex

// ---- safe mode ----
const uint64_t SAFE_MODE_NETWORK_RETRY_MS = 3600000; // networking is tried again this long after boot, 1 h
const uint64_t RESTART_COUNT_STABLE_MS = 600000;     // the restart count clears after networking has run this long, 10 min
bool networkStarted = false;    // false only in safe mode, until its retry
uint64_t networkStartedMs = 0;  // uptime when networking started

// ---- rolling log (the oldest entries are overwritten, each is stamped with the uptime) ----
const uint8_t LOG_CAPACITY = 60;     // entries kept
const uint8_t LOG_TEXT_LENGTH = 80;  // max characters per entry, including the terminator
const uint8_t TIMESTAMP_TEXT_SIZE = 24; // "+<days>d HH:MM:SS" plus terminator
struct LogEntry {
  uint64_t uptimeMs;             // uptime when logged
  char text[LOG_TEXT_LENGTH];
};
LogEntry logBuffer[LOG_CAPACITY];
uint8_t logNextIndex = 0;  // entry that the next message overwrites
uint8_t logEntryCount = 0; // entries stored so far, up to LOG_CAPACITY

// ---- API routes: command numbers ----
const uint16_t COMMAND_READ_VALUE = 2;      // value: reading below
const uint16_t COMMAND_SET_SETPOINT = 3;    // value: 50-80 F
const uint16_t COMMAND_SET_HVAC_MODE = 4;   // value: hvac mode below
const uint16_t COMMAND_SET_FAN_MODE = 5;    // value: fan mode below
const uint16_t COMMAND_SET_BACKLIGHT = 6;   // value: 0-255, 0 = off
const uint16_t COMMAND_SET_TEXT_COLOR = 7;  // value: 0-14, a place in DISPLAY_TEXT_COLORS
const uint16_t COMMAND_READ_SETTINGS = 8;   // value: 0 = all, or a setting command number
const uint16_t COMMAND_READ_LOG = 9;        // value: 0 = all entries, N = newest N
const uint16_t COMMAND_STATUS = 10;         // value: 0
const uint16_t COMMAND_STATUS_LINE = 11;    // value: 0
const uint16_t COMMAND_RESET_SETTINGS = 12; // value: 1 to confirm

// readings (value of COMMAND_READ_VALUE)
const uint16_t READ_TEMPERATURE = 0;  // control temperature, F with one decimal
const uint16_t READ_HUMIDITY = 1;     // relative humidity, % with one decimal, or NA
const uint16_t READ_SENSOR_STATE = 2; // sensor state below
const uint16_t READ_RELAYS = 3;       // "W1~Y1~G", 1 = closed
const uint16_t READ_RUNNING = 4;      // running state below

// other command values
const uint16_t READ_ALL_SETTINGS = 0;
const uint16_t RESET_SETTINGS_CONFIRM = 1;
const uint16_t STATUS_SELECTOR = 0;
const uint8_t SETTING_UNIT_TEXT_SIZE = 32; // " (heat)" after a mode in the change log

// core 0's longest normal pause, a Wi-Fi attempt and one HTTP client, must stay inside its stall
// limit. A join normally returns as soon as it is issued, but the library's wait has no firm end,
// so a join that overruns this is left to the watchdog.
static_assert(WIFI_CONNECT_TIMEOUT_MS + WIFI_DISCONNECT_SETTLE_MS + HTTP_REQUEST_DEADLINE_MS + HTTP_REPLY_DEADLINE_MS < CORE0_STALL_LIMIT_MS,
              "a Wi-Fi attempt plus one HTTP client must fit inside the core 0 stall limit");

// ---- hvac and fan mode names ----
const char *const HVAC_MODE_NAMES[] = { "off", "heat", "cool" };
const uint8_t HVAC_MODE_NAME_COUNT = sizeof(HVAC_MODE_NAMES) / sizeof(HVAC_MODE_NAMES[0]);
static_assert(HVAC_MODE_NAME_COUNT == HVAC_MODE_COOL + 1, "one name per hvac mode");
const char *const FAN_MODE_NAMES[] = { "auto", "on" };
const uint8_t FAN_MODE_NAME_COUNT = sizeof(FAN_MODE_NAMES) / sizeof(FAN_MODE_NAMES[0]);
static_assert(FAN_MODE_NAME_COUNT == FAN_MODE_ON + 1, "one name per fan mode");

// ---- setpoint range ----
const uint16_t SETPOINT_MIN_F = 50;
const uint16_t SETPOINT_MAX_F = 80;

// ---- persistent settings (each firmware build starts from the defaults, see loadSettings) ----
const uint16_t HVAC_MODE_DEFAULT = HVAC_MODE_OFF;
const uint16_t FAN_MODE_DEFAULT = FAN_MODE_AUTO;
const uint16_t SETPOINT_DEFAULT_F = 70;
const uint32_t SETTINGS_SAVE_INTERVAL_MS = 300000; // minimum time between EEPROM writes, 5 minutes
const uint16_t SETTINGS_EEPROM_SIZE = 256;         // emulated EEPROM size in bytes
const uint16_t SETTINGS_EEPROM_ADDRESS = 0;
const uint8_t EEPROM_ERASED_BYTE = 0xFF;           // value every byte is wiped to
const uint16_t SETTINGS_MAGIC = 0x5E70;            // marks a stored settings block
// compile date and time: every newly compiled upload has a different stamp, so its firmware ID
// differs from the one stored with settings saved by any earlier build
const char FIRMWARE_BUILD_STAMP[] = __DATE__ " " __TIME__;
const uint32_t FNV_OFFSET_BASIS = 2166136261u;     // 32-bit FNV-1a hash constants
const uint32_t FNV_PRIME = 16777619u;
Settings settings;       // live settings
Settings savedSettings;  // copy of what is stored in EEPROM
bool settingsSavePending = false; // true while a change is waiting to be written
uint32_t settingsLastSaveMs = 0;  // millis() of the last EEPROM write (boot counts as one)
extern "C" uint8_t _EEPROM_start[]; // the flash sector behind EEPROM, placed by the linker

// ---- activity LED ----
const uint16_t ACTIVITY_LED_BLINK_MS = 100; // on-board LED lights this long for each command
bool activityLedOn = false;                 // true while the on-board LED shows a command
uint32_t activityLedOnMs = 0;               // when the on-board LED was last lit

// ---- backlight log ----
const uint64_t BACKLIGHT_LOG_SETTLE_MS = 2000;    // a level unchanged this long has settled
const uint64_t BACKLIGHT_LOG_INTERVAL_MS = 60000; // at most one log entry per minute
bool backlightLogPending = false;                 // true while a new level waits for its log entry
uint8_t backlightLoggedLevel = DISPLAY_BRIGHTNESS_DEFAULT; // level last written to the log
uint64_t backlightLastChangeMs = 0;               // uptime when a level last arrived
uint64_t backlightLastLogMs = 0;                  // uptime of the last log entry (boot counts)

// ==== time ====

// Returns the time since boot in milliseconds from the RP2040's 64-bit microsecond timer, which
// never wraps (millis() wraps after 49.7 days, which would upset the hour-long HVAC timers).
uint64_t uptimeMs() {
  uint64_t uptimeUs = time_us_64();
  uint64_t uptimeMilliseconds = uptimeUs / US_PER_MS;
  return uptimeMilliseconds;
}

// Returns the milliseconds from sinceMs to nowMs, or 0 when sinceMs is not in the past, so a
// lockout timer can only ever run long, never short.
uint64_t msSince(uint64_t sinceMs, uint64_t nowMs) {
  bool sinceIsPast = (nowMs > sinceMs);
  if (!sinceIsPast) {
    return 0;
  }
  uint64_t elapsedMs = nowMs - sinceMs;
  return elapsedMs;
}

// Returns true once intervalMs has passed since sinceMs.
bool intervalElapsed(uint64_t sinceMs, uint64_t intervalMs, uint64_t nowMs) {
  uint64_t elapsedMs = msSince(sinceMs, nowMs);
  bool elapsed = (elapsedMs >= intervalMs);
  return elapsed;
}

// Returns how many milliseconds of intervalMs, started at sinceMs, are still to run (0 once over).
uint64_t msRemaining(uint64_t sinceMs, uint64_t intervalMs, uint64_t nowMs) {
  uint64_t elapsedMs = msSince(sinceMs, nowMs);
  bool intervalOver = (elapsedMs >= intervalMs);
  if (intervalOver) {
    return 0;
  }
  uint64_t remainingMs = intervalMs - elapsedMs;
  return remainingMs;
}

// Converts milliseconds to whole seconds for logs and status lines, e.g. 125400 -> 125.
unsigned long msToSeconds(uint64_t durationMs) {
  uint64_t wholeSeconds = durationMs / MS_PER_SECOND;
  unsigned long seconds = (unsigned long)wholeSeconds;
  return seconds;
}

// Converts a time still to run to whole seconds, rounding up so a wait never shows as 0s
// while it lasts, e.g. 400 -> 1.
unsigned long msToSecondsRoundedUp(uint64_t durationMs) {
  uint64_t roundingMs = MS_PER_SECOND - 1;
  uint64_t roundedMs = durationMs + roundingMs;
  unsigned long seconds = msToSeconds(roundedMs);
  return seconds;
}

// ==== shared state between the cores ====

// Takes stateMutex, waiting while the other core holds it. The holder may take it again
// (recursive), as logMessage() does inside a locked section. Pair every call with unlockState().
void lockState() {
  recursive_mutex_enter_blocking(&stateMutex);
}

// Releases stateMutex taken by lockState().
void unlockState() {
  recursive_mutex_exit(&stateMutex);
}

// ==== rolling log ====

// Adds one printf-style entry to the rolling log, overwriting the oldest entry when full.
// Each entry records the uptime it was logged at. Safe to call from either core.
void logMessage(const char *format, ...) {
  lockState();
  LogEntry &entry = logBuffer[logNextIndex];
  entry.uptimeMs = uptimeMs();
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.text, sizeof(entry.text), format, arguments);
  va_end(arguments);
  uint16_t followingIndex = logNextIndex + 1;
  logNextIndex = followingIndex % LOG_CAPACITY;
  if (logEntryCount < LOG_CAPACITY) {
    logEntryCount = logEntryCount + 1;
  }
  unlockState();
}

// Formats an uptime as "+<days>d HH:MM:SS".
void formatUptime(char *output, size_t outputSize, uint64_t uptimeMilliseconds) {
  uint64_t totalSeconds = uptimeMilliseconds / MS_PER_SECOND;
  uint64_t wholeDays = totalSeconds / SECONDS_PER_DAY;
  uint64_t secondsIntoDay = totalSeconds % SECONDS_PER_DAY;
  uint32_t daySeconds = (uint32_t)secondsIntoDay;
  uint32_t hours = daySeconds / SECONDS_PER_HOUR;
  uint32_t secondsIntoHour = daySeconds % SECONDS_PER_HOUR;
  uint32_t minutes = secondsIntoHour / SECONDS_PER_MINUTE;
  uint32_t seconds = secondsIntoHour % SECONDS_PER_MINUTE;
  snprintf(output, outputSize, "+%lud %02lu:%02lu:%02lu",
           (unsigned long)wholeDays, (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
}

// Formats a value held in tenths with one decimal: 713 -> "71.3", -45 -> "-4.5".
void formatTenths(char *output, size_t outputSize, int32_t tenths) {
  const char *sign = "";
  int32_t magnitude = tenths;
  bool negative = (tenths < 0);
  if (negative) {
    sign = "-";
    magnitude = -tenths;
  }
  int32_t wholePart = magnitude / TENTHS_PER_DEGREE;
  int32_t tenthPart = magnitude % TENTHS_PER_DEGREE;
  snprintf(output, outputSize, "%s%ld.%ld", sign, (long)wholePart, (long)tenthPart);
}

// ==== names ====

// Returns names[index], or the fallback when the index is outside the table.
const char *lookupName(const char *const names[], uint8_t nameCount, uint16_t index, const char *fallback) {
  bool inTable = (index < nameCount);
  if (!inTable) {
    return fallback;
  }
  return names[index];
}

// Returns the readable name of an hvac mode, e.g. 1 -> "heat".
const char *hvacModeName(uint16_t hvacMode) {
  return lookupName(HVAC_MODE_NAMES, HVAC_MODE_NAME_COUNT, hvacMode, "unknown");
}

// Returns the readable name of a fan mode, e.g. 0 -> "auto".
const char *fanModeName(uint16_t fanMode) {
  return lookupName(FAN_MODE_NAMES, FAN_MODE_NAME_COUNT, fanMode, "unknown");
}

// Returns the readable name of a text colour, e.g. 2 -> "red".
const char *textColorName(uint16_t textColor) {
  bool inTable = (textColor < DISPLAY_TEXT_COLOR_COUNT);
  if (!inTable) {
    return "unknown";
  }
  return DISPLAY_TEXT_COLORS[textColor].name;
}


// ==== persistent settings ====

// Returns the 16-bit sum of a run of bytes, the checksum used by the settings and the sensor.
uint16_t sumBytes(const uint8_t *bytes, size_t length) {
  uint16_t byteSum = 0;
  for (size_t byteIndex = 0; byteIndex < length; byteIndex++) {
    byteSum = byteSum + bytes[byteIndex];
  }
  return byteSum;
}

// Returns the checksum of every settings byte before the checksum field.
uint16_t calculateSettingsChecksum(const Settings &candidate) {
  const uint8_t *settingsBytes = (const uint8_t *)&candidate;
  size_t checksummedLength = offsetof(Settings, checksum);
  return sumBytes(settingsBytes, checksummedLength);
}

// Returns true when every setting value is within its allowed range.
bool settingValuesInRange(const Settings &candidate) {
  bool hvacModeKnown = (candidate.hvacMode <= HVAC_MODE_COOL);
  bool fanModeKnown = (candidate.fanMode <= FAN_MODE_ON);
  bool setpointAboveMin = (candidate.setpointF >= SETPOINT_MIN_F);
  bool setpointBelowMax = (candidate.setpointF <= SETPOINT_MAX_F);
  bool textColorKnown = (candidate.textColor < DISPLAY_TEXT_COLOR_COUNT);
  bool coolShutOffKnown = (candidate.coolShutOff <= 1);
  bool valuesInRange = hvacModeKnown && fanModeKnown && setpointAboveMin && setpointBelowMax && textColorKnown && coolShutOffKnown;
  return valuesInRange;
}

// Returns the ID of this firmware build: a 32-bit FNV-1a hash of its compile date and time.
uint32_t calculateFirmwareId() {
  uint32_t hash = FNV_OFFSET_BASIS;
  size_t stampLength = strlen(FIRMWARE_BUILD_STAMP);
  for (size_t characterIndex = 0; characterIndex < stampLength; characterIndex++) {
    uint8_t character = (uint8_t)FIRMWARE_BUILD_STAMP[characterIndex];
    uint32_t mixedHash = hash ^ character;
    hash = mixedHash * FNV_PRIME;
  }
  return hash;
}

// Returns true when a settings block was saved by this firmware build, is intact and every
// value is within its allowed range.
bool settingsAreValid(const Settings &candidate) {
  uint16_t expectedChecksum = calculateSettingsChecksum(candidate);
  uint32_t thisFirmwareId = calculateFirmwareId();
  bool hasMagic = (candidate.magic == SETTINGS_MAGIC);
  bool savedByThisFirmware = (candidate.firmwareId == thisFirmwareId);
  bool checksumMatches = (candidate.checksum == expectedChecksum);
  bool blockIsIntact = hasMagic && savedByThisFirmware && checksumMatches;
  bool valuesInRange = settingValuesInRange(candidate);
  bool allValid = blockIsIntact && valuesInRange;
  return allValid;
}

// Fills a settings block with the default values (off, fan auto, 70 F, white text), including its firmware
// ID, marker and checksum.
void applyDefaultSettings(Settings &target) {
  target.firmwareId = calculateFirmwareId();
  target.magic = SETTINGS_MAGIC;
  target.hvacMode = (uint8_t)HVAC_MODE_DEFAULT;
  target.fanMode = (uint8_t)FAN_MODE_DEFAULT;
  target.setpointF = SETPOINT_DEFAULT_F;
  target.textColor = DISPLAY_TEXT_COLOR_DEFAULT;
  target.coolShutOff = 0;
  target.checksum = calculateSettingsChecksum(target);
}

// Writes the live settings to flash and reads them back, because EEPROM.commit() reports success
// without checking the flash. Returns true when the flash holds them. Asking for the data pointer
// marks the RAM copy as changed, so a retry writes again even though its bytes are already there.
bool writeSettingsToFlash() {
  EEPROM.put(SETTINGS_EEPROM_ADDRESS, settings);
  EEPROM.getDataPtr();
  bool committed = EEPROM.commit();
  const uint8_t *storedBytes = _EEPROM_start + SETTINGS_EEPROM_ADDRESS;
  int comparison = memcmp(storedBytes, &settings, sizeof(Settings));
  bool storedMatches = (comparison == 0);
  bool written = committed && storedMatches;
  return written;
}

// Erases every byte of the emulated EEPROM and stores the live settings in it, so nothing saved
// by another firmware build survives. Returns true when the flash write succeeded.
bool wipeSettingsStorage() {
  for (uint16_t address = 0; address < SETTINGS_EEPROM_SIZE; address++) {
    EEPROM.write(address, EEPROM_ERASED_BYTE);
  }
  bool written = writeSettingsToFlash();
  return written;
}

// Restores the settings this firmware build saved in EEPROM. Settings are never carried over from
// another build: on the first boot after an upload (or when the block is missing or damaged) the
// EEPROM is wiped and the defaults are stored, so the thermostat comes up off. Boot counts as a
// write, so the first save can happen 5 minutes after power-up.
void loadSettings() {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);
  EEPROM.get(SETTINGS_EEPROM_ADDRESS, settings);
  bool storedSettingsValid = settingsAreValid(settings);
  if (storedSettingsValid) {
    const char *modeName = hvacModeName(settings.hvacMode);
    const char *fanName = fanModeName(settings.fanMode);
    logMessage("settings restored: mode %s, fan %s, setpoint %u F", modeName, fanName, settings.setpointF);
  } else {
    applyDefaultSettings(settings);
    bool wiped = wipeSettingsStorage();
    if (wiped) {
      logMessage("no settings from this firmware: EEPROM wiped, using defaults (off)");
    } else {
      logMessage("EEPROM wipe failed, using defaults (off)");
    }
  }
  savedSettings = settings;
  settingsLastSaveMs = millis();
}

// Marks the settings as changed, so they are written on the next allowed save.
void markSettingsChanged() {
  settings.checksum = calculateSettingsChecksum(settings);
  settingsSavePending = true;
}

// Writes pending settings to EEPROM, at most once per SETTINGS_SAVE_INTERVAL_MS to limit flash
// wear. Several quick changes cost one write, and nothing is written when the settings were
// changed back to what is already stored. The caller holds stateMutex (see saveSettingsWhenDue).
void writeSettingsWhenDue() {
  if (!settingsSavePending) {
    return;
  }
  uint32_t msSinceLastSave = millis() - settingsLastSaveMs;
  bool saveIntervalElapsed = (msSinceLastSave >= SETTINGS_SAVE_INTERVAL_MS);
  if (!saveIntervalElapsed) {
    return;
  }
  settingsSavePending = false;
  int comparison = memcmp(&settings, &savedSettings, sizeof(Settings));
  bool matchesStoredCopy = (comparison == 0);
  if (matchesStoredCopy) {
    return;
  }
  bool written = writeSettingsToFlash();
  if (written) {
    savedSettings = settings;
    logMessage("settings saved to EEPROM");
  } else {
    settingsSavePending = true; // retry after the next interval
    logMessage("settings save to EEPROM failed, retrying in 5 min");
  }
  settingsLastSaveMs = millis();
}

// Saves pending settings when due (see writeSettingsWhenDue) under stateMutex; called from
// core 0. The flash write pauses core 1 for its few tens of milliseconds.
void saveSettingsWhenDue() {
  lockState();
  writeSettingsWhenDue();
  unlockState();
}

// ==== activity LED ====

// Lights the on-board LED to show a command ran; updateActivityLed() turns it off again, so
// nothing waits. A command arriving while it is lit restarts the blink.
void blinkActivityLed() {
  if (!activityLedOn) {
    digitalWrite(LED_BUILTIN, HIGH);
  }
  activityLedOn = true;
  activityLedOnMs = millis();
}

// Turns the on-board LED off once it has been lit for ACTIVITY_LED_BLINK_MS; called from core 0's loop.
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

// ==== Wi-Fi ====

// Sets Wi-Fi client mode with the hostname from creds.h and starts connecting to the configured
// network. beginNoBlock() normally returns as soon as the join is issued and skips the library's
// wait for an address (begin() would hold core 0 for up to 30 s); the join and DHCP finish in the
// background and logWiFiStateChanges() reports the connection. WIFI_CONNECT_TIMEOUT_MS limits the
// library's retries when the join can't be issued. Runs on core 0 only, so it never holds up the
// HVAC on core 1.
void startWiFiJoin() {
  WiFi.mode(WIFI_STA);
  // the hostname must be set before WiFi.beginNoBlock so the DHCP request carries it
  WiFi.setHostname(hostname);
  // disable power management by clearing the power-save mode bits
  uint32_t powerManagementMode = CYW43_DEFAULT_PM & ~WIFI_POWER_SAVE_BITS;
  cyw43_wifi_pm(&cyw43_state, powerManagementMode);
  WiFi.setTimeout(WIFI_CONNECT_TIMEOUT_MS);
  WiFi.beginNoBlock(ssid, ssid_pwd);
  wifiLastAttemptMs = millis();
}

// Logs and starts one connection attempt (see startWiFiJoin).
void connectWiFi() {
  logMessage("connecting to Wi-Fi %s", ssid);
  startWiFiJoin();
}

// Logs when the Wi-Fi connection is made or lost, checked once a second. A connection clears the
// reconnect attempts and cancels a pending restart; a loss is logged as an error while the HVAC
// carries on.
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
    wifiHasConnected = true;
    bool restartWasPending = wifiRestartPending;
    wifiReconnectAttempts = 0;
    wifiRestartPending = false;
    wifiRestartWaitLogged = false;
    IPAddress localAddress = WiFi.localIP();
    String addressText = localAddress.toString(); // kept alive while its text is logged
    long signalStrength = (long)WiFi.RSSI();
    logMessage("Wi-Fi connected, IP %s, RSSI %ld dBm", addressText.c_str(), signalStrength);
    if (restartWasPending) {
      logMessage("Wi-Fi back: restart cancelled");
    }
  } else {
    wifiLostSinceMs = uptimeMs();
    logMessage("ERROR Wi-Fi connection lost, HVAC keeps running");
  }
}

// Returns true from boot until Wi-Fi first connects, for at most WIFI_STARTUP_WINDOW_MS. A first
// join can fail with the access point up (seen after a power cut), so waiting the full
// WIFI_RETRY_INTERVAL_MS would leave the thermostat unreachable for minutes.
bool wifiInStartupWindow() {
  if (wifiHasConnected) {
    return false;
  }
  bool windowOver = intervalElapsed(bootMs, WIFI_STARTUP_WINDOW_MS, uptimeMs());
  return !windowOver;
}

// Drops whatever the last attempt left and starts a new one, without logging it.
void retryWiFiQuietly() {
  WiFi.disconnect();
  delay(WIFI_DISCONNECT_SETTLE_MS);
  startWiFiJoin();
}

// Starts a new attempt once WIFI_STARTUP_RETRY_INTERVAL_MS has passed since the last ended. Only
// the first is logged, so they can't fill the rolling log; "Wi-Fi connected" reports the success.
// Each normally holds core 0 for about 0.5 s; the HVAC on core 1 carries on meanwhile.
void retryWiFiAtStartup(uint32_t msSinceAttempt) {
  bool retryIsDue = (msSinceAttempt >= WIFI_STARTUP_RETRY_INTERVAL_MS);
  if (!retryIsDue) {
    return;
  }
  if (!wifiStartupRetryLogged) {
    wifiStartupRetryLogged = true;
    unsigned long intervalSeconds = msToSeconds(WIFI_STARTUP_RETRY_INTERVAL_MS);
    logMessage("Wi-Fi not connected yet: retrying every %lus until 5 min after boot", intervalSeconds);
  }
  retryWiFiQuietly();
}

// Retries a lost connection every WIFI_RETRY_INTERVAL_MS, up to WIFI_RECONNECT_ATTEMPTS_MAX times
// (the first retry comes as soon as a working connection drops). Once every attempt has failed it
// asks for a restart, which restartWhenCycleEnds() carries out, and keeps retrying without logging
// while a long heat or cool cycle holds the restart up, so the thermostat comes back with the
// access point. An attempt normally holds core 0 for about 0.5 s; the HVAC on core 1 carries on
// meanwhile. Until Wi-Fi first connects after boot the quick attempts come first (see
// retryWiFiAtStartup) and don't count towards the 10.
void reconnectWiFiWhenDue() {
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  if (connectedNow) {
    return;
  }
  uint32_t msSinceAttempt = millis() - wifiLastAttemptMs;
  if (wifiInStartupWindow()) {
    retryWiFiAtStartup(msSinceAttempt);
    return;
  }
  bool retryIsDue = (msSinceAttempt >= WIFI_RETRY_INTERVAL_MS);
  if (!retryIsDue) {
    return;
  }
  if (wifiRestartPending) {
    retryWiFiQuietly();
    return;
  }
  bool attemptsUsedUp = (wifiReconnectAttempts >= WIFI_RECONNECT_ATTEMPTS_MAX);
  if (attemptsUsedUp) {
    wifiRestartPending = true;
    logMessage("ERROR Wi-Fi not back after %u attempts: restart once heat and cool are off",
               WIFI_RECONNECT_ATTEMPTS_MAX);
    return;
  }
  wifiReconnectAttempts = wifiReconnectAttempts + 1;
  logMessage("Wi-Fi reconnect attempt %u of %u", wifiReconnectAttempts, WIFI_RECONNECT_ATTEMPTS_MAX);
  WiFi.disconnect();
  delay(WIFI_DISCONNECT_SETTLE_MS);
  connectWiFi();
}

// ==== Wi-Fi restart ====

// Restarts the Pico once every Wi-Fi reconnect attempt has failed, as soon as the heat or cool
// cycle has finished (W1 and Y1 open and the blower run-on over) and no settings change is waiting
// for its EEPROM write. stateMutex is held from the
// check to the restart, so core 1 can't start a cycle in between; every relay is opened first
// (the reset opens them anyway), and the cause is left in a watchdog scratch register for the
// next boot to log. Called from core 0.
void restartWhenCycleEnds() {
  if (!wifiRestartPending) {
    return;
  }
  lockState();
  bool cycleRunning = hvacCycleRunning();
  bool savePending = settingsSavePending;
  bool mustWait = cycleRunning || savePending;
  if (mustWait) {
    if (!wifiRestartWaitLogged) {
      logMessage("Wi-Fi restart waiting for heat/cool, blower run-on and EEPROM write");
      wifiRestartWaitLogged = true;
    }
    unlockState();
    return;
  }
  logMessage("restarting: Wi-Fi not reconnected");
  openAllRelays();
  restartWithWiFiCause();
}

// Reads why the Pico last restarted (see readRestartCause) and logs it; called once from setup().
void logRestartCause() {
  readRestartCause();
  if (restartedByWatchdog) {
    logMessage("ERROR SmartThermo restarted by the watchdog (stall), relays open");
  } else if (restartedForWiFi) {
    logMessage("ERROR SmartThermo restarted: Wi-Fi not reconnected after %u attempts", WIFI_RECONNECT_ATTEMPTS_MAX);
  } else {
    logMessage("SmartThermo started, relays open");
  }
}

// ==== status and log replies ====

// Appends the hvac and fan modes, the setpoint line (see appendSetpointStatus) and whether an
// EEPROM write is pending.
void appendSettingsStatus() {
  const char *modeName = hvacModeName(settings.hvacMode);
  const char *fanName = fanModeName(settings.fanMode);
  appendBodyField("hvac mode:", "%u (%s)\n", settings.hvacMode, modeName);
  appendBodyField("fan mode:", "%u (%s)\n", settings.fanMode, fanName);
  appendSetpointStatus();
  appendBodyField("text color:", "%u (%s)\n", settings.textColor, textColorName(settings.textColor));
  if (settingsSavePending) {
    uint32_t msSinceLastSave = millis() - settingsLastSaveMs;
    uint32_t secondsUntilWrite = 0;
    bool intervalRemaining = (msSinceLastSave < SETTINGS_SAVE_INTERVAL_MS);
    if (intervalRemaining) {
      uint32_t msUntilWrite = SETTINGS_SAVE_INTERVAL_MS - msSinceLastSave;
      secondsUntilWrite = msUntilWrite / MS_PER_SECOND;
    }
    appendBodyField("EEPROM:", "pending, writes in %lus\n", (unsigned long)secondsUntilWrite);
  } else {
    appendBodyField("EEPROM:", "saved\n");
  }
}

// Reads the connection text the status shows. RSSI is a call to the Wi-Fi chip, so this runs before
// the status takes stateMutex (see runCommand): a slow chip must not hold up the HVAC on core 1.
void refreshWiFiStatusText() {
  if (!wifiConnected) {
    return;
  }
  IPAddress localAddress = WiFi.localIP();
  String addressText = localAddress.toString(); // kept alive while its text is copied
  long signalStrength = (long)WiFi.RSSI();
  snprintf(wifiStatusText, sizeof(wifiStatusText), "connected, IP %s, RSSI %ld dBm", addressText.c_str(), signalStrength);
}

// Appends the Wi-Fi connection (or the reconnect attempts and a pending restart), the uptime and
// why the Pico last started.
void appendNetworkStatus(uint64_t nowMs) {
  if (wifiConnected) {
    appendBodyField("Wi-Fi:", "%s\n", wifiStatusText);
  } else if (wifiRestartPending) {
    appendBodyField("Wi-Fi:", "not connected after %u attempts, restart once heat and cool are off\n",
                    WIFI_RECONNECT_ATTEMPTS_MAX);
  } else {
    unsigned long secondsLost = msToSeconds(msSince(wifiLostSinceMs, nowMs));
    appendBodyField("Wi-Fi:", "not connected for %lus, reconnect attempt %u of %u\n",
                    secondsLost, wifiReconnectAttempts, WIFI_RECONNECT_ATTEMPTS_MAX);
  }
  char uptimeText[TIMESTAMP_TEXT_SIZE];
  formatUptime(uptimeText, sizeof(uptimeText), nowMs);
  const char *startCause = "power-up or reset";
  if (restartedByWatchdog) {
    startCause = "watchdog restart";
  } else if (restartedForWiFi) {
    startCause = "Wi-Fi restart";
  }
  appendBodyField("uptime:", "%s, after %s\n", uptimeText, startCause);
}

// Appends every current error, one per line under "errors:", or "errors: none".
void appendErrorStatus(uint64_t nowMs) {
  bool sensorError = sensorHasError();
  bool wifiDown = !wifiConnected;
  bool interlocksOverrode = interlocksHaveOverridden();
  bool runLimitErrors = runLimitsHaveError();
  bool coolShutOff = (settings.coolShutOff != 0);
  bool anyError = sensorError || wifiDown || interlocksOverrode || runLimitErrors || coolShutOff || restartedByWatchdog || restartedForWiFi;
  if (!anyError) {
    appendBodyField("errors:", "none");
    return;
  }
  appendBody("errors:");
  appendSensorErrors(nowMs);
  if (coolShutOff) {
    appendBody("\n  cool was switched off by a sensor failure: set the hvac mode to clear this");
  }
  if (runLimitErrors) {
    appendRunLimitErrors(nowMs);
  }
  if (wifiDown) {
    appendBody("\n  Wi-Fi not connected, HVAC running without it");
  }
  if (interlocksOverrode) {
    appendInterlockErrors();
  }
  if (restartedByWatchdog) {
    appendBody("\n  last start was a watchdog restart (firmware stalled), %u in a row", watchdogRestartsInARow);
  }
  if (restartedForWiFi) {
    appendBody("\n  last start was a Wi-Fi restart (not reconnected after %u attempts)", WIFI_RECONNECT_ATTEMPTS_MAX);
  }
}

// Replies with the full status (command 10).
void writeStatus() {
  startBodyReply();
  uint64_t nowMs = uptimeMs();
  appendSensorStatus(nowMs);
  appendSettingsStatus();
  appendEquipmentStatus(nowMs);
  appendNetworkStatus(nowMs);
  appendErrorStatus(nowMs);
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

// ==== API routes ====

// Replies with the control temperature with one decimal, e.g. "71.3" (the last reading while
// stale), or "NA" while there is no usable reading.
void replyWithTemperature() {
  char temperatureText[TEMPERATURE_TEXT_SIZE];
  formatTemperatureReading(temperatureText, sizeof(temperatureText));
  replyWithText(temperatureText);
}

// Replies with the humidity with one decimal, e.g. "45.2", or "NA" while there is no usable reading.
void replyWithHumidity() {
  char humidityText[TEMPERATURE_TEXT_SIZE];
  formatHumidityReading(humidityText, sizeof(humidityText));
  replyWithText(humidityText);
}

// Replies with the relays as "W1~Y1~G", 1 = closed, e.g. "0~1~1" while cooling.
void replyWithRelays() {
  RelayOutputs relays = currentRelays();
  int heatClosed = relays.heatW1;
  int coolClosed = relays.coolY1;
  int fanClosed = relays.fanG;
  replyWithFormat("%d~%d~%d", heatClosed, coolClosed, fanClosed);
}

// Readings (command 2): 0 temperature, 1 humidity, 2 sensor state, 3 relays, 4 running state.
// Unknown readings leave the reply empty (400 ERR).
void runReadCommand(uint16_t reading) {
  switch (reading) {
    case READ_TEMPERATURE:
      replyWithTemperature();
      break;
    case READ_HUMIDITY:
      replyWithHumidity();
      break;
    case READ_SENSOR_STATE:
      replyWithNumber(currentSensorState());
      break;
    case READ_RELAYS:
      replyWithRelays();
      break;
    case READ_RUNNING:
      replyWithNumber(runningState());
      break;
    default:
      break;
  }
}

// Finishes an accepted settings change: schedules the EEPROM save, wakes the display's backlight,
// logs the change and replies with the new value. The thermostat on core 1 acts on it in its next pass.
void completeSettingChange(const char *settingName, uint16_t newValue, const char *unit) {
  markSettingsChanged();
  wakeDisplay();
  logMessage("http: %s set to %u%s", settingName, newValue, unit);
  int16_t replyValue = (int16_t)newValue;
  replyWithNumber(replyValue);
}

// Sets the setpoint (command 3), 50-80 F; other values leave the reply empty (400 ERR).
void setSetpoint(uint16_t requestedF) {
  bool aboveMin = (requestedF >= SETPOINT_MIN_F);
  bool belowMax = (requestedF <= SETPOINT_MAX_F);
  bool valid = aboveMin && belowMax;
  if (!valid) {
    return;
  }
  settings.setpointF = requestedF;
  completeSettingChange("setpoint", settings.setpointF, " F");
}

// Sets the hvac mode (command 4): 0 off, 1 heat, 2 cool; other values are rejected (400 ERR).
// Off opens W1 and Y1 at once; a change between heat and cool lets the running stage finish its
// minimum on time, then waits out the dead band. Cool is refused (400 ERR) while the sensor has
// failed. Any accepted mode clears the error left by a sensor failure switching cool off, and
// every run limit in force.
void setHvacMode(uint16_t requestedMode) {
  bool valid = (requestedMode <= HVAC_MODE_COOL);
  if (!valid) {
    return;
  }
  bool coolRequested = (requestedMode == HVAC_MODE_COOL);
  bool sensorFailed = (currentSensorState() == SENSOR_STATE_FAILED);
  if (coolRequested && sensorFailed) {
    logMessage("ERROR http: cool mode refused, DHT22 failed");
    return;
  }
  settings.hvacMode = (uint8_t)requestedMode;
  settings.coolShutOff = 0;
  clearRunLimits();
  char nameText[SETTING_UNIT_TEXT_SIZE];
  snprintf(nameText, sizeof(nameText), " (%s)", hvacModeName(settings.hvacMode));
  completeSettingChange("hvac mode", settings.hvacMode, nameText);
}

// Sets the fan mode (command 5): 0 auto, 1 on; other values are rejected (400 ERR).
void setFanMode(uint16_t requestedMode) {
  bool valid = (requestedMode <= FAN_MODE_ON);
  if (!valid) {
    return;
  }
  settings.fanMode = (uint8_t)requestedMode;
  char nameText[SETTING_UNIT_TEXT_SIZE];
  snprintf(nameText, sizeof(nameText), " (%s)", fanModeName(settings.fanMode));
  completeSettingChange("fan mode", settings.fanMode, nameText);
}

// Sets the display backlight's default level (command 6), 0 off to 255 full; other values are
// rejected (400 ERR). The level isn't saved to EEPROM: every start comes back at the default. A slider sends
// many levels, so the log entry is left to logSettledBacklight().
void setBacklight(uint16_t requestedLevel) {
  bool valid = (requestedLevel <= DISPLAY_BRIGHTNESS_MAX);
  if (!valid) {
    return;
  }
  setDisplayDefaultBrightness((uint8_t)requestedLevel);
  backlightLogPending = true;
  backlightLastChangeMs = uptimeMs();
  int16_t replyValue = (int16_t)requestedLevel;
  replyWithNumber(replyValue);
}

// Logs the backlight's default level once it has stopped changing for BACKLIGHT_LOG_SETTLE_MS, at
// most once per BACKLIGHT_LOG_INTERVAL_MS (boot counts as a log), so a dragged slider costs one log
// entry and can't push older ones out. A level dragged back to the one last logged writes nothing.
// Called from core 0's loop.
void logSettledBacklight() {
  if (!backlightLogPending) {
    return;
  }
  uint64_t nowMs = uptimeMs();
  bool levelSettled = intervalElapsed(backlightLastChangeMs, BACKLIGHT_LOG_SETTLE_MS, nowMs);
  bool logAllowed = intervalElapsed(backlightLastLogMs, BACKLIGHT_LOG_INTERVAL_MS, nowMs);
  if (!levelSettled || !logAllowed) {
    return;
  }
  backlightLogPending = false;
  uint8_t settledLevel = displayDefaultBrightness();
  bool alreadyLogged = (settledLevel == backlightLoggedLevel);
  if (alreadyLogged) {
    return;
  }
  backlightLoggedLevel = settledLevel;
  backlightLastLogMs = nowMs;
  logMessage("http: default backlight set to %u", settledLevel);
}

// Sets the display's text colour (command 7), 0-14, a place in DISPLAY_TEXT_COLORS; other values
// are rejected (400 ERR). Saved to EEPROM like the other settings.
void setTextColor(uint16_t requestedColor) {
  bool valid = (requestedColor < DISPLAY_TEXT_COLOR_COUNT);
  if (!valid) {
    return;
  }
  settings.textColor = requestedColor;
  char nameText[SETTING_UNIT_TEXT_SIZE];
  snprintf(nameText, sizeof(nameText), " (%s)", textColorName(settings.textColor));
  completeSettingChange("text color", settings.textColor, nameText);
}

// Reads settings (command 8): 0 replies "mode~fan~setpoint", a setting's command number (3-5), the
// backlight's (6) or the text colour's (7) replies with that value. Unknown selectors leave the
// reply empty (400 ERR).
void replyWithSettings(uint16_t selector) {
  switch (selector) {
    case READ_ALL_SETTINGS:
      replyWithFormat("%d~%d~%d", settings.hvacMode, settings.fanMode, settings.setpointF);
      break;
    case COMMAND_SET_SETPOINT:
      replyWithNumber(settings.setpointF);
      break;
    case COMMAND_SET_HVAC_MODE:
      replyWithNumber(settings.hvacMode);
      break;
    case COMMAND_SET_FAN_MODE:
      replyWithNumber(settings.fanMode);
      break;
    case COMMAND_SET_BACKLIGHT:
      replyWithNumber(displayDefaultBrightness());
      break;
    case COMMAND_SET_TEXT_COLOR:
      replyWithNumber(settings.textColor);
      break;
    default:
      break;
  }
}

// Replies with the one-line status (command 11, value 0) for clients that read everything in one
// request: "mode~fan~setpoint~temperature~humidity~sensor~W1~Y1~G~running",
// e.g. "1~0~70~69.4~41.0~0~1~0~0~1" while heating; temperature and humidity are NA without a usable reading.
void replyWithStatusLine(uint16_t selector) {
  bool validSelector = (selector == STATUS_SELECTOR);
  if (!validSelector) {
    return;
  }
  char temperatureText[TEMPERATURE_TEXT_SIZE];
  char humidityText[TEMPERATURE_TEXT_SIZE];
  formatTemperatureReading(temperatureText, sizeof(temperatureText));
  formatHumidityReading(humidityText, sizeof(humidityText));
  RelayOutputs relays = currentRelays();
  int heatClosed = relays.heatW1;
  int coolClosed = relays.coolY1;
  int fanClosed = relays.fanG;
  int16_t sensorStateNow = currentSensorState();
  int16_t runningNow = runningState();
  replyWithFormat("%d~%d~%d~%s~%s~%d~%d~%d~%d~%d",
                  settings.hvacMode, settings.fanMode, settings.setpointF, temperatureText, humidityText,
                  sensorStateNow, heatClosed, coolClosed, fanClosed, runningNow);
}

// Full status (command 10, value 0); other values are invalid (400 ERR).
void runStatusCommand(uint16_t selector) {
  bool validSelector = (selector == STATUS_SELECTOR);
  if (!validSelector) {
    return;
  }
  writeStatus();
}

// Resets every setting to its default, off / fan auto / 70 F / white text (command 12, value 1 to confirm;
// anything else is rejected with 400 ERR). Off opens W1 and Y1 at once; the reset is saved to
// EEPROM like any other change, and the reply lists the restored settings.
void resetSettingsToDefaults(uint16_t confirmation) {
  bool confirmed = (confirmation == RESET_SETTINGS_CONFIRM);
  if (!confirmed) {
    return;
  }
  applyDefaultSettings(settings);
  clearRunLimits();
  markSettingsChanged();
  wakeDisplay();
  logMessage("http: settings reset to defaults (off, fan auto, %u F)", settings.setpointF);
  replyWithSettings(READ_ALL_SETTINGS);
}

// The API route table: blinks the on-board LED and reads the Wi-Fi text for a status (both are calls
// to the Wi-Fi chip, so they come before the lock), then runs one decoded command (the HTTP server's
// command handler, see startHttpServer) with stateMutex held. The reply buffers belong to core 0, so
// http.cpp sends the reply after the lock is released.
void runCommand(uint16_t command, uint16_t value) {
  blinkActivityLed();
  bool statusRequested = (command == COMMAND_STATUS);
  if (statusRequested) {
    refreshWiFiStatusText();
  }
  lockState();
  switch (command) {
    case COMMAND_READ_VALUE:
      runReadCommand(value);
      break;
    case COMMAND_SET_SETPOINT:
      setSetpoint(value);
      break;
    case COMMAND_SET_HVAC_MODE:
      setHvacMode(value);
      break;
    case COMMAND_SET_FAN_MODE:
      setFanMode(value);
      break;
    case COMMAND_SET_BACKLIGHT:
      setBacklight(value);
      break;
    case COMMAND_SET_TEXT_COLOR:
      setTextColor(value);
      break;
    case COMMAND_READ_SETTINGS:
      replyWithSettings(value);
      break;
    case COMMAND_READ_LOG:
      writeLog(value);
      break;
    case COMMAND_STATUS:
      runStatusCommand(value);
      break;
    case COMMAND_STATUS_LINE:
      replyWithStatusLine(value);
      break;
    case COMMAND_RESET_SETTINGS:
      resetSettingsToDefaults(value);
      break;
    default:
      replyWithNumber(0);
      break;
  }
  unlockState();
}

// ==== safe mode ====

// Starts Wi-Fi and the API; called from setup(), or from safe mode when its hour is up.
void startNetwork() {
  connectWiFi();
  delay(STARTUP_SETTLE_MS);
  startHttpServer(runCommand);
  networkStarted = true;
  networkStartedMs = uptimeMs();
}

// Ends safe mode's hour without networking by trying it again, with core 0 watched again. A stall
// now counts on top of the restarts before it, so it comes straight back to safe mode.
void leaveSafeModeWhenDue() {
  bool retryIsDue = intervalElapsed(bootMs, SAFE_MODE_NETWORK_RETRY_MS, uptimeMs());
  if (!retryIsDue) {
    return;
  }
  logMessage("safe mode: trying Wi-Fi again");
  resumeCore0StallCheck();
  pinMode(LED_BUILTIN, OUTPUT);
  startNetwork();
}

// Forgets the watchdog restarts in a row once networking has run for RESTART_COUNT_STABLE_MS.
void clearRestartCountWhenStable() {
  if (watchdogRestartsInARow == 0) {
    return;
  }
  bool stable = intervalElapsed(networkStartedMs, RESTART_COUNT_STABLE_MS, uptimeMs());
  if (stable) {
    clearWatchdogRestartCount();
  }
}

// ==== main entry points ====

// Core 0 start-up: opens every relay first, then restores settings and prepares the sensor and
// the HVAC timers with stateMutex held, starts the watchdog, hands the HVAC to core 1 and only
// then starts networking and the display, so neither delays the HVAC. Nothing heats or cools
// until POWER_UP_DELAY_MS has passed and the sensor has read. After SAFE_MODE_RESTART_COUNT
// watchdog restarts in a row it starts in safe mode: no networking, and no use of the Wi-Fi chip
// (the on-board LED is on it), so a fault there can't stop the HVAC getting past its delay.
void setup() {
  recursive_mutex_init(&stateMutex);
  lockState();
  bootMs = uptimeMs();
  initRelays();
  logRestartCause();
  bool safeMode = (watchdogRestartsInARow >= SAFE_MODE_RESTART_COUNT);
  if (safeMode) {
    unsigned long retrySeconds = msToSeconds(SAFE_MODE_NETWORK_RETRY_MS);
    logMessage("ERROR safe mode: %u watchdog restarts in a row, no Wi-Fi for %lus", watchdogRestartsInARow, retrySeconds);
    pauseCore0StallCheck();
  } else {
    // the on-board LED stays lit while starting up
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
  }
  loadSettings();
  initThermostat();
  wifiLostSinceMs = bootMs;
  unsigned long powerUpDelaySeconds = msToSeconds(POWER_UP_DELAY_MS);
  logMessage("heat and cool wait %lus after power-up", powerUpDelaySeconds);
  unlockState();
  startWatchdog();
  controlReady = true;
  if (!safeMode) {
    startNetwork();
  }
  startDisplay();
  if (!safeMode) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

// Core 0 loop: Wi-Fi, the HTTP server, the display and the EEPROM saves; in safe mode only the
// last two. A Wi-Fi attempt and one HTTP client are the longest pauses (see the budget check near
// the top), and none of them holds up the HVAC on core 1.
void loop() {
  markCore0Alive();
  if (networkStarted) {
    logWiFiStateChanges();
    reconnectWiFiWhenDue();
    restartWhenCycleEnds();
    updateActivityLed();
    serviceHttpClient();
    clearRestartCountWhenStable();
  } else {
    leaveSafeModeWhenDue();
  }
  updateDisplay();
  logSettledBacklight();
  saveSettingsWhenDue();
}

// Core 1 start-up: waits until setup() on core 0 has opened the relays and prepared the shared state.
void setup1() {
  while (!controlReady) {
    tight_loop_contents();
  }
}

// Core 1 loop: feeds the watchdog, then reads the sensor and runs the thermostat with stateMutex
// held. The pause between passes leaves core 0 room to take the mutex.
void loop1() {
  feedWatchdogWhileCore0Runs();
  lockState();
  readSensorWhenDue();
  updateThermostat();
  unlockState();
  delay(CORE1_LOOP_PAUSE_MS);
}
