// =====================================================================================
// test/sim_scenarios.cpp
// SmartThermo host test: the scenarios. Each one scripts a timeline (minutes from its start) of
// API requests, faults and power events, with expectations on the API replies, the relays (both
// the firmware's view and the pins), the firmware log, the EEPROM flash and the restarts. The
// safety rules in sim_checks.cpp run underneath every scenario all the time.
//
// Timing: events in the step window (T - step, T] run before both cores' passes at T, so a request
// at minute 10.0 is acted on by the pass at 10.0. Expectations look a few seconds after the moment
// a timer ends (the firmware's timers are listed in the API reference's timing summary).
// =====================================================================================
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "../thermo.h"
#include "../display.h"
#include "sim_world.h"

extern uint32_t simDisplayWakes; // sim_hal.cpp, how many times the display's backlight was woken

const uint16_t SIM_SETTINGS_MAGIC = 0x5E70; // SmartThermo.ino SETTINGS_MAGIC
const uint32_t FOREIGN_FIRMWARE_ID = 0x0BADF00D;

// scenarioValues slots
const int VALUE_MAX_SENSOR_STATE = 0;
const int VALUE_W1_ON_US = 1;
const int VALUE_WINDOW_US = 2;
const int VALUE_RESET_DELAY_S = 3;

// ==== timeline helpers ====

static uint64_t minutesToUs(double minutes) {
  double microseconds = minutes * (double)US_PER_MINUTE_SIM;
  return (uint64_t)llround(microseconds);
}

// True in the one step whose window holds the given minute.
static bool at(double minute) {
  uint64_t eventUs = minutesToUs(minute);
  bool inWindow = (world->stepFromUs < eventUs) && (eventUs <= world->stepToUs);
  return inWindow;
}

// True while the step ends inside [fromMinute, toMinute).
static bool during(double fromMinute, double toMinute) {
  uint64_t stepEndUs = world->stepToUs;
  bool inside = (stepEndUs > minutesToUs(fromMinute)) && (stepEndUs <= minutesToUs(toMinute));
  return inside;
}

static uint64_t stepDurationUs() {
  return world->stepToUs - world->stepFromUs;
}

// Records one scenario expectation: traced as "ok", or counted as a failure with its text.
void scenarioExpect(bool passed, const char *format, ...) {
  char text[FAILURE_TEXT_LENGTH];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  world->scenarioChecks = world->scenarioChecks + 1;
  if (passed) {
    tracef("EXPECT", "ok: %s", text);
    return;
  }
  world->scenarioFailures = world->scenarioFailures + 1;
  checkFail("scenario", "%s", text);
}

static bool contains(const std::string &text, const char *part) {
  bool found = (text.find(part) != std::string::npos);
  return found;
}

// A user's request over Wi-Fi, e.g. api(4, 1) sends ?4,0,1~. Traced with the reply; answers
// "NO ANSWER" while the Pico is off, Wi-Fi is down or core 0 hangs (the request times out).
static std::string api(uint16_t command, uint16_t value) {
  char request[24];
  snprintf(request, sizeof(request), "?%u,0,%u~", command, value);
  const char *unreachable = nullptr;
  if (!world->firmwareUp) {
    unreachable = "Pico off";
  } else if (!simWiFiConnected()) {
    unreachable = "Wi-Fi down";
  } else if (world->nowUs < world->core0StallUntilUs) {
    unreachable = "core 0 hung";
  }
  if (unreachable != nullptr) {
    tracef("API", "%s -> no answer (%s)", request, unreachable);
    return "NO ANSWER";
  }
  world->day.apiRequests = world->day.apiRequests + 1;
  world->total.apiRequests = world->total.apiRequests + 1;
  std::string reply = simRunCommand(command, value);
  bool multiline = contains(reply, "\n");
  if (multiline) {
    tracef("API", "%s ->", request);
    traceMultiline("API", reply);
  } else {
    tracef("API", "%s -> %s", request, reply.c_str());
  }
  drainFirmwareLog();
  return reply;
}

// A read for the test itself (a debug probe: no network, not traced).
static std::string probe(uint16_t command, uint16_t value) {
  if (!world->firmwareUp) {
    return "NO ANSWER";
  }
  std::string reply = simRunCommand(command, value);
  drainFirmwareLog();
  return reply;
}

static void expectReply(uint16_t command, uint16_t value, const std::string &expected) {
  std::string reply = api(command, value);
  scenarioExpect(reply == expected, "?%u,0,%u~ replies \"%s\" (got \"%s\")", command, value, expected.c_str(), reply.c_str());
}

static void expectProbe(uint16_t command, uint16_t value, const std::string &expected) {
  std::string reply = probe(command, value);
  scenarioExpect(reply == expected, "?%u,0,%u~ reads \"%s\" (got \"%s\")", command, value, expected.c_str(), reply.c_str());
}

// The relays as "W1~Y1~G" from the pins.
static std::string pinsText() {
  char text[8];
  snprintf(text, sizeof(text), "%d~%d~%d", simRelayClosed(SIM_PIN_W1) ? 1 : 0, simRelayClosed(SIM_PIN_Y1) ? 1 : 0,
           simRelayClosed(SIM_PIN_G) ? 1 : 0);
  return std::string(text);
}

// Expects the relays, e.g. "0~1~1" while cooling, from the API (?2,0,3~) and on the pins.
static void expectRelays(const char *expected) {
  std::string reported = probe(2, 3);
  std::string driven = pinsText();
  bool matches = (reported == expected) && (driven == expected);
  scenarioExpect(matches, "relays W1~Y1~G are %s (API says %s, pins drive %s)", expected, reported.c_str(), driven.c_str());
}

static void expectSensorState(int state) {
  char expected[8];
  snprintf(expected, sizeof(expected), "%d", state);
  expectProbe(2, 2, expected);
}

static void expectStatusContains(const char *part) {
  std::string status = probe(10, 0);
  bool found = contains(status, part);
  scenarioExpect(found, "status (?10,0,0~) contains \"%s\"", part);
  if (!found) {
    traceMultiline("STATUS", status);
  }
}

static void expectLogged(double fromMinute, const char *text) {
  bool found = firmwareLogContains(fromMinute, text);
  scenarioExpect(found, "firmware logged \"%s\" (since minute %.1f)", text, fromMinute);
}

static Settings flashSettings() {
  Settings stored;
  memcpy(&stored, world->flash, sizeof(stored));
  return stored;
}

// Expects the settings block in the EEPROM flash.
static void expectFlash(int mode, int fanMode, int setpointF) {
  Settings stored = flashSettings();
  bool hasMagic = (stored.magic == SIM_SETTINGS_MAGIC);
  bool matches = hasMagic && (stored.hvacMode == mode) && (stored.fanMode == fanMode) && (stored.setpointF == setpointF);
  scenarioExpect(matches, "EEPROM flash holds mode %d, fan %d, setpoint %d (has magic %04X, mode %u, fan %u, setpoint %u)",
                 mode, fanMode, setpointF, stored.magic, stored.hvacMode, stored.fanMode, stored.setpointF);
}

static bool flashErased(size_t fromByte, size_t toByte) {
  for (size_t index = fromByte; index < toByte; index++) {
    if (world->flash[index] != 0xFF) {
      return false;
    }
  }
  return true;
}

// True for a reading with one decimal, e.g. "71.3" or "-4.5".
static bool isTenths(const std::string &text) {
  size_t position = 0;
  if (position < text.size() && text[position] == '-') {
    position = position + 1;
  }
  size_t digitsBefore = 0;
  while (position < text.size() && isdigit((unsigned char)text[position])) {
    position = position + 1;
    digitsBefore = digitsBefore + 1;
  }
  bool hasPoint = (position < text.size()) && (text[position] == '.');
  bool oneDecimal = hasPoint && (position + 2 == text.size()) && isdigit((unsigned char)text[position + 1]);
  return (digitsBefore > 0) && oneDecimal;
}

static int countLines(const std::string &text) {
  if (text.empty()) {
    return 0;
  }
  int lines = 1;
  for (char character : text) {
    if (character == '\n') {
      lines = lines + 1;
    }
  }
  return lines;
}

static std::vector<std::string> splitFields(const std::string &text, char delimiter) {
  std::vector<std::string> fields;
  std::string field;
  for (char character : text) {
    if (character == delimiter) {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(character);
    }
  }
  fields.push_back(field);
  return fields;
}

static const char *faultDescription(SensorFault fault) {
  switch (fault) {
    case SENSOR_FAULT_NONE:
      return "working again";
    case SENSOR_FAULT_UNPLUGGED:
      return "UNPLUGGED (no answer, line held high by the pull-up)";
    case SENSOR_FAULT_DATA_TIMEOUT:
      return "stops part way through its frame (data timeout)";
    case SENSOR_FAULT_BAD_CHECKSUM:
      return "noise flips a bit on every frame (bad checksum)";
    case SENSOR_FAULT_OUT_OF_RANGE:
      return "sends garbage with a valid checksum (out of range)";
    case SENSOR_FAULT_STUCK_LOW:
      return "data line shorted to ground (no response)";
    default:
      return "?";
  }
}

static void setSensorFault(SensorFault fault) {
  world->dht.fault = fault;
  tracef("EVENT", "DHT22 %s", faultDescription(fault));
}

// Cuts the power for offMinutes: the Pico stops at once, every relay opens, and it boots again.
static void powerCut(double offMinutes) {
  if (!world->firmwareUp) {
    return;
  }
  world->powerOffUs = minutesToUs(offMinutes);
  tracef("EVENT", "POWER CUT for %.2f min", offMinutes);
  SimReset reset;
  reset.kind = RESET_POWER_LOSS;
  throw reset;
}

// Hangs one core's loop for a while (a firmware stall); only the watchdog gets it going again.
static void hangCore(int core, double seconds) {
  if (!world->firmwareUp) {
    return;
  }
  uint64_t untilUs = world->nowUs + (uint64_t)(seconds * (double)US_PER_SECOND_SIM);
  if (core == 0) {
    world->core0StallUntilUs = untilUs;
  } else {
    world->core1StallUntilUs = untilUs;
  }
  tracef("EVENT", "core %d HANGS for %.0f s (%s stops)", core, seconds, (core == 0) ? "Wi-Fi/API loop" : "HVAC loop");
}

static void setModeAndSetpoint(int mode, int setpointF) {
  expectReply(3, (uint16_t)setpointF, std::to_string(setpointF));
  expectReply(4, (uint16_t)mode, std::to_string(mode));
}

// The firmware's sensor state, read directly each step (-1 while the Pico is off).
static int sensorStateNow() {
  if (!world->firmwareUp) {
    return -1;
  }
  return currentSensorState();
}

static void trackMaxSensorState() {
  int state = sensorStateNow();
  bool afterStartup = (world->firmwareUp) && (simUptimeUs() > SPEC_SENSOR_STARTUP_US);
  if (afterStartup && (double)state > world->scenarioValues[VALUE_MAX_SENSOR_STATE]) {
    world->scenarioValues[VALUE_MAX_SENSOR_STATE] = state;
  }
}

// Adds this step to the W1 duty measurement while the step ends inside the window.
static void measureHeatDuty(double fromMinute, double toMinute) {
  if (!during(fromMinute, toMinute)) {
    return;
  }
  uint64_t stepUs = stepDurationUs();
  world->scenarioValues[VALUE_WINDOW_US] = world->scenarioValues[VALUE_WINDOW_US] + (double)stepUs;
  if (simRelayClosed(SIM_PIN_W1)) {
    world->scenarioValues[VALUE_W1_ON_US] = world->scenarioValues[VALUE_W1_ON_US] + (double)stepUs;
  }
}

static double measuredHeatDutyPercent() {
  double windowUs = world->scenarioValues[VALUE_WINDOW_US];
  if (windowUs <= 0.0) {
    return 0.0;
  }
  return 100.0 * world->scenarioValues[VALUE_W1_ON_US] / windowUs;
}

// ==== startup ====

static void startupEvents() {
  if (at(0.05)) {
    expectRelays("0~0~0");
    expectSensorState(SIM_SENSOR_OK);
    expectLogged(0, "SmartThermo started, relays open");
    expectLogged(0, "no settings from this firmware: EEPROM wiped, using defaults (off)");
    expectLogged(0, "heat and cool wait 300s after power-up");
    expectReply(8, 0, "0~0~70");
  }
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
    expectReply(7, 3, "3");
  }
  if (at(1.0)) {
    expectStatusContains("call waiting: power-up delay");
    expectStatusContains("text color:          3 (orange)");
    expectRelays("0~0~0");
  }
  if (at(4.9)) {
    expectRelays("0~0~0");
  }
  if (at(5.1)) {
    expectRelays("1~0~0");
    expectProbe(2, 4, "1");
    expectLogged(4.9, "heat on (W1 closed)");
  }
  if (at(6.0)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 70);
    Settings stored = flashSettings();
    scenarioExpect(stored.textColor == 3, "EEPROM flash holds text colour 3 (has %u)", stored.textColor);
    expectLogged(4.9, "settings saved to EEPROM");
  }
}

static void startupFinish() {
  scenarioExpect(world->total.heatStarts >= 1, "the furnace ran (%u starts)", world->total.heatStarts);
}

// ==== every API selection ====

// Sends every command with every valid value and a set of invalid ones, checking each reply.
static void sweepEveryApiSelection() {
  // readings (command 2)
  std::string temperature = api(2, 0);
  scenarioExpect(isTenths(temperature), "?2,0,0~ temperature has one decimal (got \"%s\")", temperature.c_str());
  std::string humidity = api(2, 1);
  scenarioExpect(isTenths(humidity), "?2,0,1~ humidity has one decimal (got \"%s\")", humidity.c_str());
  expectReply(2, 2, "0");
  expectReply(2, 3, "0~0~0");
  expectReply(2, 4, "0");
  const uint16_t invalidReadings[] = { 5, 6, 10, 100, 9999 };
  for (uint16_t reading : invalidReadings) {
    expectReply(2, reading, "400 ERR");
  }
  // setpoint (command 3): every whole degree from 50 to 80, then values outside the range
  for (uint16_t setpointF = 50; setpointF <= 80; setpointF++) {
    expectReply(3, setpointF, std::to_string(setpointF));
    expectReply(8, 3, std::to_string(setpointF));
  }
  const uint16_t invalidSetpoints[] = { 0, 1, 49, 81, 100, 255, 9999 };
  for (uint16_t setpointF : invalidSetpoints) {
    expectReply(3, setpointF, "400 ERR");
  }
  expectReply(8, 3, "80");
  // the rolling log (command 9), while the setpoint changes are the newest entries
  std::string fullLog = api(9, 0);
  int fullLines = countLines(fullLog);
  scenarioExpect(contains(fullLog, "http: setpoint set to 80 F"), "?9,0,0~ log holds the last setpoint change");
  int expectedLines = (world->firmwareLineCount < 60) ? (int)world->firmwareLineCount : 60;
  scenarioExpect(fullLines == expectedLines, "?9,0,0~ returns every stored entry, %d (got %d)", expectedLines, fullLines);
  std::string oneLine = api(9, 1);
  scenarioExpect(countLines(oneLine) == 1 && contains(oneLine, "setpoint set to 80 F"), "?9,0,1~ returns the newest entry only");
  scenarioExpect(countLines(api(9, 5)) == 5, "?9,0,5~ returns 5 entries");
  scenarioExpect(countLines(api(9, 9999)) == fullLines, "?9,0,9999~ returns every stored entry");
  // fan mode (command 5)
  expectReply(5, 1, "1");
  expectReply(8, 5, "1");
  expectReply(5, 0, "0");
  expectReply(8, 5, "0");
  expectReply(5, 2, "400 ERR");
  expectReply(5, 9999, "400 ERR");
  expectReply(8, 5, "0");
  // hvac mode (command 4)
  expectReply(4, 2, "2");
  expectReply(8, 4, "2");
  expectReply(4, 1, "1");
  expectReply(8, 4, "1");
  expectReply(4, 0, "0");
  expectReply(8, 4, "0");
  expectReply(4, 3, "400 ERR");
  expectReply(4, 9999, "400 ERR");
  expectReply(8, 4, "0");
  // display backlight's default level (command 6)
  expectReply(8, 6, std::to_string(DISPLAY_BRIGHTNESS_DEFAULT));
  const uint16_t backlightLevels[] = { 0, 1, 128, 255 };
  for (uint16_t level : backlightLevels) {
    expectReply(6, level, std::to_string(level));
    expectReply(8, 6, std::to_string(level));
  }
  expectReply(6, 256, "400 ERR");
  expectReply(6, 9999, "400 ERR");
  expectReply(8, 6, "255");
  // display text colour (command 7)
  expectReply(8, 7, "0");
  for (uint16_t color = 0; color < DISPLAY_TEXT_COLOR_COUNT; color++) {
    expectReply(7, color, std::to_string(color));
    expectReply(8, 7, std::to_string(color));
  }
  expectReply(7, DISPLAY_TEXT_COLOR_COUNT, "400 ERR");
  expectReply(7, 9999, "400 ERR");
  expectReply(8, 7, "14");
  expectReply(7, 0, "0");
  // only an accepted settings change wakes the display's backlight
  uint32_t wakesBefore = simDisplayWakes;
  expectReply(3, 80, "80");
  scenarioExpect(simDisplayWakes == wakesBefore + 1, "a setpoint change wakes the backlight");
  wakesBefore = simDisplayWakes;
  expectReply(4, 3, "400 ERR");
  expectReply(8, 3, "80");
  expectReply(6, 255, "255");
  scenarioExpect(simDisplayWakes == wakesBefore, "a refused value, a read and the backlight level don't wake it");
  // settings (command 8)
  expectReply(8, 0, "0~0~80");
  const uint16_t invalidSelectors[] = { 1, 2, 9, 9999 };
  for (uint16_t selector : invalidSelectors) {
    expectReply(8, selector, "400 ERR");
  }
  // full status (command 10)
  std::string status = api(10, 0);
  const char *statusLabels[] = { "temperature:", "humidity:", "sensor:", "hvac mode:", "fan mode:", "setpoint:", "text color:", "EEPROM:",
                                 "running:", "relays:", "heat:", "cool:", "compressor:", "starts last hour:", "Wi-Fi:",
                                 "uptime:", "errors:" };
  for (const char *label : statusLabels) {
    scenarioExpect(contains(status, label), "?10,0,0~ status has \"%s\"", label);
  }
  expectReply(10, 1, "400 ERR");
  // one-line status (command 11)
  std::string statusLine = api(11, 0);
  std::vector<std::string> fields = splitFields(statusLine, '~');
  bool fieldsOk = (fields.size() == 10) && (fields[0] == "0") && (fields[1] == "0") && (fields[2] == "80") && isTenths(fields[3]) &&
                  isTenths(fields[4]) && (fields[5] == "0") && (fields[6] == "0") && (fields[7] == "0") && (fields[8] == "0") &&
                  (fields[9] == "0");
  scenarioExpect(fieldsOk, "?11,0,0~ is mode~fan~setpoint~temp~humidity~sensor~W1~Y1~G~running (got \"%s\")", statusLine.c_str());
  expectReply(11, 1, "400 ERR");
  // reset (command 12)
  expectReply(12, 0, "400 ERR");
  expectReply(12, 2, "400 ERR");
  expectReply(12, 1, "0~0~70");
  expectReply(8, 0, "0~0~70");
  // command numbers with no route answer 0
  const uint16_t unknownCommands[] = { 0, 1, 13, 99, 9999 };
  for (uint16_t command : unknownCommands) {
    expectReply(command, 0, "0");
  }
}

static void apiSelectionsEvents() {
  if (at(0.5)) {
    sweepEveryApiSelection();
  }
  // heat, then a change to cool while the furnace is inside its 3 min minimum
  if (at(10)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 74);
  }
  if (at(10.2)) {
    expectRelays("1~0~0");
  }
  if (at(11)) {
    setModeAndSetpoint(SIM_MODE_COOL, 62);
  }
  if (at(12.5)) {
    expectRelays("1~0~0");
    expectLogged(10.9, "heat mode changed, held for furnace minimum on time");
    expectStatusContains("call waiting: heat/cool dead band");
  }
  if (at(13.2)) {
    expectRelays("0~0~0");
    expectLogged(10.9, "cool call waiting: heat/cool dead band");
  }
  if (at(17.9)) {
    expectRelays("0~0~0");
  }
  if (at(18.2)) {
    expectRelays("0~1~1");
    expectLogged(17.9, "cool on (G, Y1 closed)");
  }
  // back to heat while the compressor is inside its 5 min minimum: run-on, then the dead band
  if (at(19)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 80);
  }
  if (at(22.9)) {
    expectRelays("0~1~1");
    expectLogged(18.9, "cool mode changed, held for compressor minimum on time");
  }
  if (at(23.2)) {
    expectRelays("0~0~1");
  }
  if (at(24.2)) {
    expectRelays("0~0~0");
    expectLogged(23.9, "fan off (G opened): fan auto, idle");
  }
  if (at(27.9)) {
    expectRelays("0~0~0");
  }
  if (at(28.2)) {
    expectRelays("1~0~0");
  }
  // off stops the furnace at once, even inside its minimum on time
  if (at(29)) {
    expectReply(4, SIM_MODE_OFF, "0");
  }
  if (at(29.1)) {
    expectRelays("0~0~0");
    expectLogged(28.9, "heat off (W1 opened) after 60s: mode off");
  }
  // fan on with mode off: fan only
  if (at(30)) {
    expectReply(5, SIM_FAN_ON, "1");
  }
  if (at(30.1)) {
    expectRelays("0~0~1");
    expectProbe(2, 4, "3");
  }
  if (at(32)) {
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(32.1)) {
    expectRelays("0~0~0");
    expectProbe(2, 4, "0");
  }
  // cool, then off inside the compressor's minimum on time: Y1 and G open at once, no run-on
  if (at(34)) {
    setModeAndSetpoint(SIM_MODE_COOL, 64);
  }
  if (at(34.3)) {
    expectRelays("0~1~1");
  }
  if (at(35)) {
    expectReply(4, SIM_MODE_OFF, "0");
  }
  if (at(35.1)) {
    expectRelays("0~0~0");
    expectLogged(34.9, "cool off (Y1, G opened) after 60s: mode off");
  }
  // cool again: the compressor's 5 min off time holds it
  if (at(36)) {
    expectReply(4, SIM_MODE_COOL, "2");
  }
  if (at(36.1)) {
    expectRelays("0~0~0");
    expectStatusContains("call waiting: compressor off time");
    expectStatusContains("off-time lockout");
    expectLogged(35.9, "cool call waiting: compressor off time");
  }
  if (at(39.9)) {
    expectRelays("0~0~0");
  }
  if (at(40.2)) {
    expectRelays("0~1~1");
  }
  // reset to defaults: off, fan auto, 70 F; saved like any change
  if (at(41)) {
    expectReply(12, 1, "0~0~70");
  }
  if (at(41.1)) {
    expectRelays("0~0~0");
  }
  if (at(47)) {
    expectFlash(SIM_MODE_OFF, SIM_FAN_AUTO, 70);
  }
  if (at(50)) {
    expectReply(4, SIM_MODE_HEAT, "1");
  }
}

// ==== fan modes ====

static void fanCoolEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 76);
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(5.2)) {
    expectRelays("0~1~1");
  }
  if (at(90)) {
    expectReply(5, SIM_FAN_ON, "1");
  }
  if (at(90.1)) {
    std::string relays = probe(2, 3);
    scenarioExpect(relays.size() == 5 && relays[4] == '1', "fan on: G closed (relays %s)", relays.c_str());
  }
  if (at(150)) {
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(200)) {
    expectReply(4, SIM_MODE_OFF, "0");
  }
  if (at(200.1)) {
    expectRelays("0~0~0");
  }
  if (at(210)) {
    expectReply(5, SIM_FAN_ON, "1");
  }
  if (at(210.1)) {
    expectRelays("0~0~1");
    expectProbe(2, 4, "3");
  }
  if (at(215)) {
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(215.1)) {
    expectRelays("0~0~0");
  }
}

static void fanCoolFinish() {
  scenarioExpect(world->total.coolStarts >= 2, "the compressor cycled (%u starts)", world->total.coolStarts);
  scenarioExpect(world->checks.runOnsObserved >= 1, "the blower ran on 60 s after a cool cycle (%u times)",
                 world->checks.runOnsObserved);
}

static void fanHeatEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(5.2)) {
    expectRelays("1~0~0");
  }
  if (at(60)) {
    expectReply(5, SIM_FAN_ON, "1");
  }
  if (at(60.1)) {
    std::string relays = probe(2, 3);
    scenarioExpect(relays.size() == 5 && relays[4] == '1', "fan on with heat: G closed (relays %s)", relays.c_str());
  }
  if (at(120)) {
    expectReply(4, SIM_MODE_OFF, "0");
  }
  if (at(120.1)) {
    expectRelays("0~0~1");
    expectProbe(2, 4, "3");
  }
  if (at(150)) {
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(150.1)) {
    expectRelays("0~0~0");
    expectProbe(2, 4, "0");
  }
  if (at(160)) {
    expectReply(4, SIM_MODE_HEAT, "1");
  }
}

static void fanHeatFinish() {
  scenarioExpect(world->total.heatStarts >= 2, "the furnace cycled (%u starts)", world->total.heatStarts);
}

// ==== sensor errors ====

static void sensorMissingPrepare() {
  world->dht.fault = SENSOR_FAULT_UNPLUGGED;
  tracef("EVENT", "DHT22 %s from power-up", faultDescription(SENSOR_FAULT_UNPLUGGED));
}

static void sensorMissingEvents() {
  if (at(0.3)) {
    expectReply(3, 70, "70");
    expectReply(5, SIM_FAN_ON, "1");
    expectSensorState(SIM_SENSOR_STARTING);
    expectReply(2, 0, "NA");
    expectReply(2, 1, "NA");
  }
  if (at(0.5)) {
    expectRelays("0~0~1");
  }
  if (at(1.05)) {
    expectSensorState(SIM_SENSOR_LOCKOUT);
    expectRelays("0~0~0");
    expectLogged(0, "ERROR DHT22 not readable at startup (no response): HVAC locked out");
    expectStatusContains("DHT22 not readable at startup (no response): HVAC locked out, every relay open");
    expectStatusContains("no reading since boot");
    std::string statusLine = api(11, 0);
    scenarioExpect(contains(statusLine, "~NA~NA~2~0~0~0~0"), "?11,0,0~ shows NA readings and the lockout (got \"%s\")",
                   statusLine.c_str());
  }
  if (at(11)) {
    expectRelays("0~0~0"); // mode off: the lockout outlasts the 10 min, with G open too
    expectSensorState(SIM_SENSOR_LOCKOUT);
    expectReply(4, SIM_MODE_HEAT, "1");
  }
  if (at(11.1)) {
    expectSensorState(SIM_SENSOR_FAILED);
    expectRelays("1~0~1");
    expectLogged(10.9, "ERROR DHT22 failed, no reading for 600s (no response)");
    expectStatusContains("no reading since boot");
    expectReply(4, SIM_MODE_COOL, "400 ERR");
  }
  if (at(15.2)) {
    expectRelays("0~0~1"); // the fallback's on part ended at 15 min, 5 min after the failure
  }
  if (at(30)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(30.2)) {
    expectSensorState(SIM_SENSOR_OK);
    expectLogged(29.9, "DHT22 reading again");
    expectRelays("1~0~1");
  }
}

// A dead sensor followed by a restart, with heat mode saved: locked out at first, then the 25 % fallback.
static void sensorDeadRestartEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(6)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 70);
  }
  if (at(10)) {
    setSensorFault(SENSOR_FAULT_UNPLUGGED);
  }
  if (at(10.5)) {
    powerCut(0.5);
  }
  if (at(12.2)) {
    expectSensorState(SIM_SENSOR_LOCKOUT);
    expectRelays("0~0~0");
    expectProbe(8, 4, "1");
    expectLogged(10.9, "ERROR DHT22 not readable at startup (no response): HVAC locked out");
  }
  if (at(20.8)) {
    expectSensorState(SIM_SENSOR_LOCKOUT);
    expectRelays("0~0~0");
  }
  if (at(21.2)) {
    expectSensorState(SIM_SENSOR_FAILED);
    expectRelays("1~0~0");
    expectLogged(20.9, "ERROR DHT22 failed, no reading for 600s (no response)");
    expectLogged(20.9, "ERROR DHT22 failed: heat running 25%");
    expectLogged(20.9, "heat on (W1 closed): sensor failed, 25% fallback");
    expectStatusContains("no reading since boot");
  }
  if (at(26.3)) {
    expectRelays("0~0~0");
    expectLogged(25.9, "heat off (W1 opened) after 300s: fallback off time");
  }
}

static void sensorIntermittentPrepare() {
  world->dht.intermittentPercent = 8;
  tracef("EVENT", "DHT22 cable noise: 8 %% of frames fail (bad checksum or data timeout)");
}

static void sensorIntermittentEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  trackMaxSensorState();
}

static void sensorIntermittentFinish() {
  double faultedPercent = 100.0 * (double)world->dht.faultedReads / (double)world->dht.reads;
  scenarioExpect(faultedPercent > 5.0 && faultedPercent < 11.0, "about 8 %% of reads failed (%.1f %%)", faultedPercent);
  scenarioExpect(world->scenarioValues[VALUE_MAX_SENSOR_STATE] == SIM_SENSOR_OK,
                 "single failed reads never made the sensor stale (worst state %.0f)", world->scenarioValues[VALUE_MAX_SENSOR_STATE]);
  expectLogged(0, "DHT22 read failed");
}

// The compressor runs from minute 5. Unplugged at 20 (last good reading 19.95), the sensor goes
// stale at 20.95: the compressor, long past its minimum, stops at once with its 60 s run-on, and
// the cool call waits for a fresh reading; once it reads, the compressor off time (to 25.95) holds it.
static void sensorStaleEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 75);
  }
  if (at(20)) {
    setSensorFault(SENSOR_FAULT_UNPLUGGED);
  }
  if (at(20.5)) {
    expectSensorState(SIM_SENSOR_OK);
    expectRelays("0~1~1");
  }
  if (at(21.2)) {
    expectSensorState(SIM_SENSOR_STALE);
    std::string temperature = api(2, 0);
    scenarioExpect(isTenths(temperature), "stale: ?2,0,0~ still gives the last reading (got \"%s\")", temperature.c_str());
    expectStatusContains("heat/cool paused, fails in");
    expectStatusContains("no response (");
    expectLogged(19.9, "ERROR DHT22 no reading for 60s (no response), heat/cool paused");
    expectLogged(19.9, "DHT22 read failed: no response");
    expectLogged(20.9, "cool off (Y1 opened)");
    scenarioExpect(firmwareLogContains(20.9, "sensor stale"), "the compressor stop names the stale sensor");
    expectRelays("0~0~1");
  }
  if (at(22.2)) {
    expectRelays("0~0~0");
  }
  if (at(23)) {
    expectStatusContains("call waiting: sensor stale");
    expectLogged(20.9, "cool call waiting: sensor stale");
    expectRelays("0~0~0");
  }
  if (at(24)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(24.2)) {
    expectSensorState(SIM_SENSOR_OK);
    expectLogged(23.9, "DHT22 reading again");
    expectLogged(23.9, "cool call waiting: compressor off time");
  }
  if (at(25.8)) {
    expectRelays("0~0~0");
  }
  if (at(26.2)) {
    expectRelays("0~1~1");
  }
  if (at(90)) {
    setSensorFault(SENSOR_FAULT_UNPLUGGED);
  }
  if (at(93)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(93.2)) {
    expectSensorState(SIM_SENSOR_OK);
  }
  trackMaxSensorState();
}

static void sensorStaleFinish() {
  scenarioExpect(world->scenarioValues[VALUE_MAX_SENSOR_STATE] == SIM_SENSOR_STALE,
                 "short dropouts went stale but never failed (worst state %.0f)", world->scenarioValues[VALUE_MAX_SENSOR_STATE]);
}

// The furnace runs from minute 5. Unplugged at 30 (last good reading 29.95): stale at 30.95 stops
// the furnace and nothing starts; failed at 39.95 starts the 25 % fallback (5 min on per 20).
static void sensorFailHeatEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(29.5)) {
    expectRelays("1~0~0");
  }
  if (at(30)) {
    setSensorFault(SENSOR_FAULT_UNPLUGGED);
  }
  if (at(31.2)) {
    expectSensorState(SIM_SENSOR_STALE);
    expectRelays("0~0~0");
    expectLogged(30.9, "heat off (W1 opened)");
    scenarioExpect(firmwareLogContains(30.9, "sensor stale"), "the furnace stop names the stale sensor");
    expectStatusContains("heat/cool paused, fails in");
  }
  if (at(35) || at(39.8)) {
    expectRelays("0~0~0");
  }
  if (at(40.2)) {
    expectSensorState(SIM_SENSOR_FAILED);
    expectRelays("1~0~0");
    expectLogged(39.9, "heat on (W1 closed): sensor failed, 25% fallback");
    expectLogged(39.9, "ERROR DHT22 failed, no reading for 600s (no response)");
    expectLogged(39.9, "ERROR DHT22 failed: heat running 25% (300s every 1200s)");
    expectReply(2, 0, "NA");
    expectReply(2, 1, "NA");
  }
  if (at(41)) {
    expectStatusContains("heat running 25% fallback until the sensor reads again");
    expectStatusContains("not used: sensor failed, heat runs 25%");
    expectReply(4, SIM_MODE_COOL, "400 ERR");
    expectLogged(40.9, "ERROR http: cool mode refused, DHT22 failed");
    expectReply(8, 4, "1");
  }
  if (at(45.2)) {
    expectRelays("0~0~0");
    expectLogged(44.9, "heat off (W1 opened) after 300s: fallback off time");
  }
  measureHeatDuty(40, 120);
  if (at(150)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(150.2)) {
    expectSensorState(SIM_SENSOR_OK);
    expectLogged(149.9, "DHT22 reading again");
  }
}

static void sensorFailHeatFinish() {
  double dutyPercent = measuredHeatDutyPercent();
  scenarioExpect(dutyPercent > 22.0 && dutyPercent < 28.0, "failed sensor: furnace ran %.1f %% of 80 min (25 %% fallback)", dutyPercent);
}

// The compressor starts at minute 5 and the sensor is unplugged at 6 (last good reading 5.95). It
// goes stale at 6.95, inside the compressor's 5 min minimum, so Y1 holds to 10.0, then stops with
// its run-on. Failed at 15.95: cool mode is switched off and saved, and cool is refused until the
// sensor reads again; then cool can be set again.
const char *const COOL_SHUT_OFF_ERROR = "cool was switched off by a sensor failure";

static void sensorFailCoolEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 72);
  }
  if (at(5.2)) {
    expectRelays("0~1~1");
  }
  if (at(6)) {
    setSensorFault(SENSOR_FAULT_UNPLUGGED);
  }
  if (at(8)) {
    expectSensorState(SIM_SENSOR_STALE);
    expectRelays("0~1~1");
    expectLogged(6.9, "cool sensor stale, held for compressor minimum on time");
  }
  if (at(10.2)) {
    expectRelays("0~0~1");
    expectLogged(9.9, "cool off (Y1 opened) after 300s: sensor stale");
  }
  if (at(11.2)) {
    expectRelays("0~0~0");
  }
  if (at(16.1)) {
    expectSensorState(SIM_SENSOR_FAILED);
    expectRelays("0~0~0");
    expectReply(8, 4, "0");
    expectLogged(15.9, "ERROR DHT22 failed: cool mode switched off");
  }
  if (at(17)) {
    expectFlash(SIM_MODE_OFF, SIM_FAN_AUTO, 72);
    scenarioExpect(flashSettings().coolShutOff == 1, "the cool shut-off is saved with the mode");
  }
  if (at(20)) {
    expectReply(4, SIM_MODE_COOL, "400 ERR");
    expectStatusContains("cool not allowed until the sensor reads again");
    expectStatusContains(COOL_SHUT_OFF_ERROR);
  }
  if (at(25)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(25.2)) {
    expectSensorState(SIM_SENSOR_OK);
    expectReply(8, 4, "0");
    expectStatusContains(COOL_SHUT_OFF_ERROR); // the error outlasts the sensor's recovery
  }
  if (at(26)) {
    expectReply(4, SIM_MODE_COOL, "2");
    scenarioExpect(!contains(probe(10, 0), COOL_SHUT_OFF_ERROR), "setting the mode clears the error");
  }
  if (at(26.3)) {
    expectRelays("0~1~1");
  }
}

struct TimedFault {
  double startMinute;
  double endMinute;
  SensorFault fault;
  const char *firmwareName; // how the firmware names the read result
};

const TimedFault BAD_DATA_FAULTS[] = {
  { 10, 13, SENSOR_FAULT_DATA_TIMEOUT, "data timeout" },
  { 16, 19, SENSOR_FAULT_BAD_CHECKSUM, "bad checksum" },
  { 22, 25, SENSOR_FAULT_OUT_OF_RANGE, "reading out of range" },
  { 28, 31, SENSOR_FAULT_STUCK_LOW, "no response" },
};

static void sensorBadDataEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  for (const TimedFault &timed : BAD_DATA_FAULTS) {
    if (at(timed.startMinute)) {
      setSensorFault(timed.fault);
    }
    if (at(timed.startMinute + 2.0)) {
      char lastRead[64];
      snprintf(lastRead, sizeof(lastRead), "%s (", timed.firmwareName); // the "last read:" line, e.g. "bad checksum (3 in a row)"
      char logLine[64];
      snprintf(logLine, sizeof(logLine), "DHT22 read failed: %s", timed.firmwareName);
      expectSensorState(SIM_SENSOR_STALE);
      expectStatusContains(lastRead);
      expectLogged(timed.startMinute - 0.1, logLine);
    }
    if (at(timed.endMinute)) {
      setSensorFault(SENSOR_FAULT_NONE);
    }
    if (at(timed.endMinute + 0.2)) {
      expectSensorState(SIM_SENSOR_OK);
    }
  }
  if (at(60)) {
    setSensorFault(SENSOR_FAULT_OUT_OF_RANGE);
  }
  if (at(70.2)) {
    expectSensorState(SIM_SENSOR_FAILED);
    expectStatusContains("(reading out of range)");
    expectLogged(69.8, "ERROR DHT22 failed, no reading for 600s (reading out of range)");
  }
  if (at(75)) {
    setSensorFault(SENSOR_FAULT_NONE);
  }
  if (at(75.2)) {
    expectSensorState(SIM_SENSOR_OK);
  }
}

static void sensorSpikeEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  const double spikeMinutes[] = { 30, 60, 90, 120, 150 };
  const double spikeSizesF[] = { 20, 20, -20, -20, 40 };
  for (size_t index = 0; index < 5; index++) {
    if (at(spikeMinutes[index])) {
      world->dht.spikeF = spikeSizesF[index];
      tracef("EVENT", "DHT22 sends one valid but wrong reading, %+.0f F off", spikeSizesF[index]);
    }
    if (at(spikeMinutes[index] + 0.5)) {
      expectLogged(spikeMinutes[index] - 0.1, "DHT22 read failed: unconfirmed jump");
    }
  }
  if (at(121)) {
    bool startedOnSpike = firmwareLogContains(89.9, "heat on (W1 closed): 4") || firmwareLogContains(89.9, "heat on (W1 closed): 5");
    scenarioExpect(!startedOnSpike, "a reading 20 F low never starts the furnace");
  }
}

// ==== run limits ====

// The DHT22 keeps sending 60 F whatever the room does, in heat: three 1 h runs with 30 min rests,
// then the 25 % fallback (5 min per 20), until the reading moves again.
static void runLimitHeatFrozenPrepare() {
  world->dht.readingFrozen = true;
  world->dht.frozenReadingF = 60.0;
  tracef("EVENT", "the DHT22's reading is frozen at 60 F");
}

static void runLimitHeatFrozenEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(5.2) || at(64.8) || at(95.2) || at(154.8) || at(185.2) || at(244.8)) {
    expectRelays("1~0~0");
  }
  if (at(65.2)) {
    expectRelays("0~0~0");
    expectLogged(64.9, "ERROR heat: no temperature response for 3600s, resting 1800s");
    expectLogged(64.9, "heat off (W1 opened) after 360"); // 3600 s or 3601 s: the window starts on the pass after W1 closes
    expectLogged(64.9, ": no temperature response, 60.0 F");
    expectStatusContains("heat stopped at a run limit: resting");
    expectStatusContains("heat: no temperature response in its last run");
  }
  if (at(70)) {
    expectStatusContains("call waiting: run limit rest");
  }
  if (at(94.8) || at(155.2) || at(184.8)) {
    expectRelays("0~0~0");
  }
  if (at(245.2)) {
    expectRelays("0~0~0");
    expectLogged(244.9, "ERROR reading frozen: heat running 25% until it moves");
    expectStatusContains("reading frozen: heat running 25%");
  }
  if (at(274.8) || at(280.2) || at(294.8) || at(300.2)) {
    expectRelays("0~0~0");
  }
  if (at(275.2) || at(295.2)) {
    expectRelays("1~0~0");
  }
  if (at(275.5)) {
    expectLogged(274.9, "heat on (W1 closed): reading frozen, 25% fallback");
  }
  if (at(310)) {
    world->dht.readingFrozen = false;
    tracef("EVENT", "the DHT22 reads the room again");
  }
  if (at(310.3)) {
    expectLogged(309.9, "reading moving again: heat back to normal control");
    scenarioExpect(!contains(probe(10, 0), "reading frozen"), "the status no longer reports a frozen reading");
  }
}

// The same in cool, frozen at 85 F: three 2 h runs with 30 min rests, each stop with its blower
// run-on until the last, then cool is off and saved, until a person sets a mode.
static void runLimitCoolFrozenPrepare() {
  world->dht.readingFrozen = true;
  world->dht.frozenReadingF = 85.0;
  tracef("EVENT", "the DHT22's reading is frozen at 85 F");
}

static void runLimitCoolFrozenEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 72);
  }
  if (at(5.2) || at(124.8) || at(155.2) || at(274.8) || at(305.2) || at(424.8)) {
    expectRelays("0~1~1");
  }
  if (at(125.2)) {
    expectRelays("0~0~1");
    expectLogged(124.9, "ERROR cool: no temperature response for 7200s, resting 1800s");
  }
  if (at(126.2) || at(154.8) || at(276.2) || at(304.8)) {
    expectRelays("0~0~0");
  }
  if (at(425.2)) {
    expectRelays("0~0~0");
    expectProbe(8, 4, "0");
    expectLogged(424.9, "ERROR reading frozen: cool mode switched off");
    expectStatusContains("cool was switched off by a sensor failure");
  }
  if (at(431)) {
    expectFlash(SIM_MODE_OFF, SIM_FAN_AUTO, 72);
    scenarioExpect(flashSettings().coolShutOff == 1, "the cool shut-off is saved with the mode");
  }
  if (at(432)) {
    expectReply(4, SIM_MODE_COOL, "2");
    scenarioExpect(!contains(probe(10, 0), "cool was switched off"), "setting the mode clears the error");
  }
  if (at(432.2)) {
    expectRelays("0~1~1");
  }
}

// A furnace locked out: W1 runs 1 h, rests 30 min, repeating with the alarm showing. The room
// falling proves the sensor works, so the frozen-reading step never comes.
static void runLimitLockoutEvents() {
  if (at(0.5)) {
    world->house.furnaceLockedOut = true;
    tracef("EVENT", "the furnace locks out: it ignores W1");
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(5.2) || at(95.2) || at(185.2) || at(275.2)) {
    expectRelays("1~0~0");
  }
  if (at(65.2) || at(155.2) || at(245.2)) {
    expectRelays("0~0~0");
    expectStatusContains("heat: no temperature response in its last run");
  }
}

static void runLimitLockoutFinish() {
  scenarioExpect(!firmwareLogContains(0, "reading frozen"), "a falling room never counts as a frozen reading");
}

// ==== power and restarts ====

static void powerLossEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
    expectReply(7, 9, "9");
  }
  if (at(20)) {
    powerCut(0.5);
  }
  if (at(20.6)) {
    expectReply(8, 0, "1~0~70");
    expectReply(8, 7, "9");
    expectLogged(20.4, "settings restored: mode heat, fan auto, setpoint 70 F");
    expectLogged(20.4, "SmartThermo started, relays open");
    expectRelays("0~0~0");
  }
  if (at(25.4)) {
    expectRelays("0~0~0");
  }
  if (at(60)) {
    powerCut(120);
  }
  if (at(180.1)) {
    expectRelays("0~0~0");
  }
  if (at(185.6)) {
    expectRelays("1~0~0");
  }
}

static void powerLossFinish() {
  scenarioExpect(world->bootCount == 3 && world->powerLosses == 2, "two power cuts, three boots (got %u boots)", world->bootCount);
}

static void powerBlipCoolEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 74);
  }
  if (at(5.2)) {
    expectRelays("0~1~1");
  }
  if (at(8)) {
    powerCut(0.25);
  }
  if (at(8.4)) {
    expectReply(8, 0, "2~0~74");
    expectRelays("0~0~0");
  }
  if (at(13.1)) {
    expectRelays("0~0~0");
  }
  if (at(13.5)) {
    expectRelays("0~1~1");
  }
}

static void powerLossUnsavedEvents() {
  if (at(0.5)) {
    expectReply(4, SIM_MODE_HEAT, "1");
  }
  if (at(10)) {
    expectReply(3, 72, "72");
  }
  if (at(10.2)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 72);
  }
  if (at(11)) {
    expectReply(3, 66, "66");
  }
  if (at(12)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 72);
    expectStatusContains("pending, writes in");
    powerCut(1);
  }
  if (at(13.2)) {
    expectReply(8, 3, "72");
    expectLogged(12.9, "settings restored: mode heat, fan auto, setpoint 72 F");
  }
  if (at(20)) {
    expectReply(5, SIM_FAN_ON, "1");
  }
  if (at(20.2)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_ON, 72);
  }
  if (at(20.5)) {
    expectReply(5, SIM_FAN_AUTO, "0");
  }
  if (at(24.9)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_ON, 72);
  }
  if (at(25.2)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 72);
  }
}

// A settings block that another firmware build saved: valid magic and checksum, other ID.
static void eepromCorruptPrepare() {
  Settings foreign;
  memset(&foreign, 0, sizeof(foreign));
  foreign.firmwareId = FOREIGN_FIRMWARE_ID;
  foreign.magic = SIM_SETTINGS_MAGIC;
  foreign.hvacMode = SIM_MODE_HEAT;
  foreign.fanMode = SIM_FAN_ON;
  foreign.setpointF = 75;
  const uint8_t *bytes = (const uint8_t *)&foreign;
  uint16_t checksum = 0;
  for (size_t index = 0; index < offsetof(Settings, checksum); index++) {
    checksum = (uint16_t)(checksum + bytes[index]);
  }
  foreign.checksum = checksum;
  memcpy(world->flash, &foreign, sizeof(foreign));
  tracef("EVENT", "flash holds settings saved by another firmware build (heat, fan on, 75 F)");
}

static void eepromCorruptEvents() {
  if (at(0.1)) {
    expectReply(8, 0, "0~0~70");
    expectLogged(0, "no settings from this firmware: EEPROM wiped, using defaults (off)");
    Settings stored = flashSettings();
    scenarioExpect(stored.firmwareId != FOREIGN_FIRMWARE_ID, "the foreign block was replaced (firmware ID %08lX)",
                   (unsigned long)stored.firmwareId);
    expectFlash(SIM_MODE_OFF, SIM_FAN_AUTO, 70);
    scenarioExpect(flashErased(sizeof(Settings), sizeof(world->flash)), "the rest of the EEPROM was wiped to FF");
  }
  if (at(1)) {
    expectReply(4, SIM_MODE_HEAT, "1");
  }
  if (at(6)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 70);
    powerCut(0.5);
  }
  if (at(6.7)) {
    expectReply(8, 0, "1~0~70");
    expectLogged(6.4, "settings restored: mode heat, fan auto, setpoint 70 F");
  }
  if (at(15)) {
    world->flash[6] = (uint8_t)(world->flash[6] ^ 0x01);
    tracef("EVENT", "flash bit rot: hvac mode byte flipped (the checksum no longer matches)");
    powerCut(0.5);
  }
  if (at(15.7)) {
    expectReply(8, 0, "0~0~70");
    expectLogged(15.4, "no settings from this firmware: EEPROM wiped, using defaults (off)");
  }
}

static void eepromWriteFailPrepare() {
  world->failCommitsRemaining = 3;
  tracef("EVENT", "the next 3 flash writes fail");
}

static void eepromWriteFailEvents() {
  if (at(0.1)) {
    expectLogged(0, "EEPROM wipe failed, using defaults (off)");
  }
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(5.2)) {
    expectLogged(4.9, "settings save to EEPROM failed, retrying in 5 min");
    expectStatusContains("pending, writes in");
  }
  if (at(10.2)) {
    scenarioExpect(flashErased(0, sizeof(Settings)), "after two failed writes the flash is still erased");
  }
  if (at(15.2)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 70);
    expectLogged(14.9, "settings saved to EEPROM");
  }
  if (at(20)) {
    powerCut(0.5);
  }
  if (at(20.7)) {
    expectReply(8, 0, "1~0~70");
  }
}

// ==== Wi-Fi ====

static void wifiShortEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(20)) {
    simSetAccessPoint(false);
  }
  if (at(20.2)) {
    expectLogged(19.9, "ERROR Wi-Fi connection lost, HVAC keeps running");
    std::string reply = api(2, 2);
    scenarioExpect(reply == "NO ANSWER", "the API cannot be reached without Wi-Fi");
    expectStatusContains("Wi-Fi not connected, HVAC running without it");
  }
  measureHeatDuty(20, 34.5);
  if (at(30)) {
    expectLogged(19.9, "Wi-Fi reconnect attempt 5 of 10");
  }
  if (at(34.5)) {
    simSetAccessPoint(true);
  }
  if (at(38)) {
    expectReply(2, 2, "0");
    expectLogged(34.4, "Wi-Fi connected, IP 192.168.1.40");
    expectStatusContains("connected, IP 192.168.1.40");
  }
}

static void wifiShortFinish() {
  scenarioExpect(world->scenarioValues[VALUE_W1_ON_US] > 0.0, "the furnace kept cycling during the Wi-Fi outage (%.0f s on)",
                 world->scenarioValues[VALUE_W1_ON_US] / (double)US_PER_SECOND_SIM);
  scenarioExpect(world->bootCount == 1, "no restart for a 14.5 min outage (%u boots)", world->bootCount);
}

static void wifiRestartEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(10)) {
    simSetAccessPoint(false);
  }
  if (at(29)) {
    expectLogged(9.9, "Wi-Fi reconnect attempt 10 of 10");
  }
  if (at(100)) {
    simSetAccessPoint(true);
  }
  if (at(118)) {
    expectReply(2, 2, "0");
  }
}

static void wifiRestartFinish() {
  scenarioExpect(world->wifiRestarts >= 2, "the Pico restarted itself while Wi-Fi stayed down (%u restarts)", world->wifiRestarts);
  expectLogged(0, "ERROR Wi-Fi not back after 10 attempts: restart once heat and cool are off");
  expectLogged(0, "restarting: Wi-Fi not reconnected");
  expectLogged(0, "ERROR SmartThermo restarted: Wi-Fi not reconnected after 10 attempts");
  expectStatusContains("last start was a Wi-Fi restart");
  scenarioExpect(world->total.heatStarts >= 3, "the furnace kept running between restarts (%u starts)", world->total.heatStarts);
}

// The furnace runs throughout, holding the restart up, and the access point comes back meanwhile:
// the retries carry on, so Wi-Fi returns and the restart is cancelled.
static void wifiRestartWaitEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 80);
  }
  if (at(10)) {
    simSetAccessPoint(false);
  }
  if (at(33)) {
    expectLogged(9.9, "ERROR Wi-Fi not back after 10 attempts: restart once heat and cool are off");
    expectLogged(9.9, "Wi-Fi restart waiting for heat/cool, blower run-on and EEPROM write");
    simSetAccessPoint(true);
  }
  if (at(37)) {
    expectLogged(32.9, "Wi-Fi back: restart cancelled");
    expectReply(2, 2, "0");
    expectRelays("1~0~0");
  }
}

static void wifiRestartWaitFinish() {
  scenarioExpect(world->wifiRestarts == 0, "no restart once the access point is back (%u restarts)", world->wifiRestarts);
}

// A power cut with the access point not answering at boot: quick retries for 5 min, then every 2 min.
static void wifiStartupEvents() {
  if (at(10)) {
    simSetAccessPoint(false);
    powerCut(0.5);
  }
  if (at(11)) {
    expectLogged(10.4, "Wi-Fi not connected yet: retrying every 15s");
    simSetAccessPoint(true);
  }
  if (at(11.5)) {
    expectLogged(10.9, "Wi-Fi connected, IP 192.168.1.40");
    expectReply(2, 2, "0");
    bool countedAttempt = firmwareLogContains(10.4, "Wi-Fi reconnect attempt");
    scenarioExpect(!countedAttempt, "the quick attempts don't count towards the 10");
  }
  if (at(20)) {
    simSetAccessPoint(false);
    powerCut(0.5);
  }
  if (at(27.6)) {
    expectLogged(20.4, "Wi-Fi reconnect attempt 1 of 10");
    simSetAccessPoint(true);
  }
  if (at(30)) {
    expectLogged(27.5, "Wi-Fi connected, IP 192.168.1.40");
  }
}

static void wifiStartupFinish() {
  expectStatusContains("connected, IP 192.168.1.40");
}

// ==== watchdog ====

static void watchdogCore0Events() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
  if (at(20)) {
    hangCore(0, 120);
  }
  if (at(21)) {
    expectLogged(20.4, "ERROR SmartThermo restarted by the watchdog (stall), relays open");
    expectStatusContains("last start was a watchdog restart (firmware stalled)");
    expectRelays("0~0~0");
  }
}

static void watchdogCore0Finish() {
  double resetAfterS = ((double)world->lastWatchdogResetUs - (double)minutesToUs(20)) / (double)US_PER_SECOND_SIM;
  scenarioExpect(world->watchdogTimeouts == 1 && resetAfterS >= 30.0 && resetAfterS <= 40.0,
                 "core 0 hung: core 1 stopped feeding after 30 s and the watchdog reset 8 s later (reset %.1f s after the hang)",
                 resetAfterS);
}

// Core 0 hangs soon after every boot: the third watchdog restart starts safe mode, where the HVAC
// gets past its power-up delay and a further hang restarts nothing. Networking returns after 1 h,
// and 10 min later the restart count has cleared.
static void watchdogLoopEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 80);
  }
  if (at(6)) {
    expectFlash(SIM_MODE_HEAT, SIM_FAN_AUTO, 80);
  }
  if (at(10) || at(11) || at(12)) {
    hangCore(0, 120);
  }
  if (at(13)) {
    expectLogged(12, "ERROR safe mode: 3 watchdog restarts in a row");
    scenarioExpect(world->watchdogTimeouts == 3, "three watchdog restarts (%u)", world->watchdogTimeouts);
    scenarioExpect(api(2, 2) == "NO ANSWER", "no networking in safe mode");
  }
  if (at(14)) {
    hangCore(0, 120);
  }
  if (at(17)) {
    scenarioExpect(world->watchdogTimeouts == 3, "a hang in safe mode restarts nothing (%u restarts)", world->watchdogTimeouts);
  }
  if (at(18.5)) {
    scenarioExpect(pinsText() == "1~0~0", "the furnace runs in safe mode (relays %s)", pinsText().c_str());
  }
  if (at(75)) {
    expectLogged(72, "safe mode: trying Wi-Fi again");
    expectReply(2, 2, "0");
  }
  if (at(84)) {
    uint32_t restartCount = world->watchdogRegisters.scratch[1] & 0xFFFF;
    scenarioExpect(restartCount == 0, "the restart count clears 10 min after networking returns (%u)", restartCount);
  }
  if (at(85)) {
    hangCore(0, 120);
  }
  if (at(87)) {
    expectReply(2, 2, "0");
    expectStatusContains("1 in a row");
    scenarioExpect(!firmwareLogContains(85, "ERROR safe mode"), "one restart after a clear doesn't start safe mode");
  }
}

static void watchdogLoopFinish() {
  scenarioExpect(world->watchdogTimeouts == 4, "four watchdog restarts in all (%u)", world->watchdogTimeouts);
}

static void watchdogCore1Events() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 75);
  }
  if (at(5.2)) {
    expectRelays("0~1~1");
  }
  if (at(10)) {
    hangCore(1, 60);
  }
  if (at(10.5)) {
    expectLogged(10.0, "ERROR SmartThermo restarted by the watchdog (stall), relays open");
    expectRelays("0~0~0");
  }
  if (at(15.5)) {
    expectRelays("0~1~1");
  }
}

static void watchdogCore1Finish() {
  double resetAfterS = ((double)world->lastWatchdogResetUs - (double)minutesToUs(10)) / (double)US_PER_SECOND_SIM;
  scenarioExpect(world->watchdogTimeouts == 1 && resetAfterS >= 8.0 && resetAfterS <= 10.0,
                 "core 1 hung: the watchdog reset after 8 s (reset %.1f s after the hang)", resetAfterS);
}

// ==== short cycling ====

static void shortCycleCoolEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 72);
  }
  for (int toggle = 0; toggle <= 120; toggle++) {
    if (at(6.0 + toggle)) {
      uint16_t setpointF = (toggle % 2 == 0) ? 80 : 72;
      expectReply(3, setpointF, std::to_string(setpointF));
    }
  }
}

static void shortCycleCoolFinish() {
  scenarioExpect(world->checks.maxCoolStartsPerHour == SPEC_COOL_STARTS_PER_HOUR, "the compressor reached and held 4 starts an hour (max %u)",
                 world->checks.maxCoolStartsPerHour);
  expectLogged(0, "cool call waiting: starts per hour limit");
  expectLogged(0, "held for compressor minimum on time");
  expectLogged(0, "cool call waiting: compressor off time");
}

static void shortCycleHeatEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 76);
  }
  for (int toggle = 0; toggle <= 120; toggle++) {
    if (at(6.0 + toggle)) {
      uint16_t setpointF = (toggle % 2 == 0) ? 64 : 76;
      expectReply(3, setpointF, std::to_string(setpointF));
    }
  }
}

static void shortCycleHeatFinish() {
  scenarioExpect(world->checks.maxHeatStartsPerHour == SPEC_HEAT_STARTS_PER_HOUR, "the furnace reached and held 6 starts an hour (max %u)",
                 world->checks.maxHeatStartsPerHour);
  expectLogged(0, "heat call waiting: starts per hour limit");
  expectLogged(0, "held for furnace minimum on time");
}

// ==== extreme weather ====

static void extremeColdPrepare() {
  world->weather.active = true;
  world->weather.meanF = -12.0;
  world->weather.swingF = 10.0;
  tracef("EVENT", "polar vortex: outdoor mean -12 F");
}

static void extremeColdEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_HEAT, 70);
  }
}

static void heatWavePrepare() {
  world->weather.active = true;
  world->weather.meanF = 97.0;
  world->weather.swingF = 18.0;
  tracef("EVENT", "heat wave: outdoor mean 97 F, afternoons over 105 F");
}

static void heatWaveEvents() {
  if (at(0.5)) {
    setModeAndSetpoint(SIM_MODE_COOL, 75);
  }
}

// ==== season runs: a household's daily schedule through the API ====

struct ScheduleEntry {
  double hour;
  int mode;
  int setpointF;
};

const ScheduleEntry WINTER_SCHEDULE[] = {
  { 6.0, SIM_MODE_HEAT, 70 }, { 8.5, SIM_MODE_HEAT, 66 }, { 17.0, SIM_MODE_HEAT, 70 }, { 22.5, SIM_MODE_HEAT, 64 },
};
const ScheduleEntry SUMMER_SCHEDULE[] = {
  { 6.0, SIM_MODE_COOL, 74 }, { 8.5, SIM_MODE_COOL, 78 }, { 16.5, SIM_MODE_COOL, 73 }, { 22.5, SIM_MODE_COOL, 72 },
};
const ScheduleEntry SHOULDER_SCHEDULE[] = {
  { 6.0, SIM_MODE_HEAT, 69 }, { 10.0, SIM_MODE_OFF, 69 }, { 13.0, SIM_MODE_COOL, 71 }, { 20.0, SIM_MODE_OFF, 71 },
  { 22.0, SIM_MODE_HEAT, 64 },
};

static const ScheduleEntry *seasonSchedule(size_t &count) {
  Season season = world->config.season;
  if (season == SEASON_WINTER) {
    count = sizeof(WINTER_SCHEDULE) / sizeof(WINTER_SCHEDULE[0]);
    return WINTER_SCHEDULE;
  }
  if (season == SEASON_SUMMER) {
    count = sizeof(SUMMER_SCHEDULE) / sizeof(SUMMER_SCHEDULE[0]);
    return SUMMER_SCHEDULE;
  }
  count = sizeof(SHOULDER_SCHEDULE) / sizeof(SHOULDER_SCHEDULE[0]);
  return SHOULDER_SCHEDULE;
}

// Minutes from the scenario start to a time of day on a day (0 = the first day).
static double minuteOfClock(int64_t dayIndex, double hour) {
  double hoursFromStart = (double)dayIndex * 24.0 + hour - world->config.startHour;
  return hoursFromStart * 60.0;
}

static void applyScheduleEntry(const ScheduleEntry &entry) {
  int setpointF = entry.setpointF;
  if (world->config.setpointOverrideF > 0) {
    setpointF = world->config.setpointOverrideF;
  }
  api(4, (uint16_t)entry.mode);
  if (entry.mode != SIM_MODE_OFF) {
    api(3, (uint16_t)setpointF);
  }
}

static void applyFanPlanAtStart() {
  if (world->config.fanPlan == FAN_PLAN_ON) {
    api(5, SIM_FAN_ON);
  } else {
    api(5, SIM_FAN_AUTO);
  }
}

// The fan schedule: on for 3 hours every other day (18:00, or 12:00 in summer).
static void runFanSchedule(int64_t dayIndex) {
  if (world->config.fanPlan != FAN_PLAN_SCHEDULE) {
    return;
  }
  bool fanDay = (dayIndex % 2 == 0);
  if (!fanDay) {
    return;
  }
  double onHour = (world->config.season == SEASON_SUMMER) ? 12.0 : 18.0;
  double offHour = onHour + 3.0;
  if (at(minuteOfClock(dayIndex, onHour))) {
    api(5, SIM_FAN_ON);
  }
  if (at(minuteOfClock(dayIndex, offHour))) {
    api(5, SIM_FAN_AUTO);
  }
}

// Random realistic faults for one day (season runs with --faults): a sensor dropout of 1-25 min
// of a random kind, cable noise, a Wi-Fi outage of 2-45 min, a power cut of seconds to 90 min, a
// firmware hang on either core, a flash write failure and a wrong reading.
static void runRandomFaults(int64_t dayIndex) {
  uint64_t day = (uint64_t)dayIndex * SEASON_COUNT + (uint64_t)world->config.season;
  if (at(minuteOfClock(dayIndex, 0.0))) {
    uint32_t noisePercent = (uint32_t)(simRandom(36, day, 0) * 4.0);
    world->dht.intermittentPercent = noisePercent;
    tracef("EVENT", "DHT22 cable noise today: %u %% of frames fail", noisePercent);
  }
  if (simRandom(31, day, 0) < 0.6) {
    double startHour = 0.5 + 23.0 * simRandom(31, day, 1);
    double lengthMinutes = 1.0 + 24.0 * simRandom(31, day, 2);
    SensorFault fault = (SensorFault)(1 + (int)(simRandom(31, day, 3) * 5.0));
    double startMinute = minuteOfClock(dayIndex, startHour);
    if (at(startMinute)) {
      setSensorFault(fault);
      tracef("EVENT", "(sensor fault for %.1f min)", lengthMinutes);
    }
    if (at(startMinute + lengthMinutes)) {
      setSensorFault(SENSOR_FAULT_NONE);
    }
  }
  if (simRandom(32, day, 0) < 0.45) {
    double startMinute = minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(32, day, 1));
    double lengthMinutes = 2.0 + 43.0 * simRandom(32, day, 2);
    if (at(startMinute)) {
      simSetAccessPoint(false);
    }
    if (at(startMinute + lengthMinutes)) {
      simSetAccessPoint(true);
    }
  }
  if (simRandom(33, day, 0) < 0.3) {
    double startMinute = minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(33, day, 1));
    double lengthDraw = simRandom(33, day, 2);
    double offMinutes = 0.05 + 90.0 * lengthDraw * lengthDraw;
    if (at(startMinute)) {
      powerCut(offMinutes);
    }
  }
  if (simRandom(34, day, 0) < 0.12) {
    if (at(minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(34, day, 1)))) {
      hangCore(0, 45);
    }
  }
  if (simRandom(37, day, 0) < 0.08) {
    if (at(minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(37, day, 1)))) {
      hangCore(1, 20);
    }
  }
  if (simRandom(35, day, 0) < 0.15) {
    if (at(minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(35, day, 1)))) {
      world->failCommitsRemaining = 1 + (uint32_t)(simRandom(35, day, 2) * 2.0);
      tracef("EVENT", "the next %u flash writes fail", world->failCommitsRemaining);
    }
  }
  if (simRandom(38, day, 0) < 0.3) {
    if (at(minuteOfClock(dayIndex, 0.5 + 23.0 * simRandom(38, day, 1)))) {
      double sizeF = 5.0 + 20.0 * simRandom(38, day, 2);
      if (simRandom(38, day, 3) < 0.5) {
        sizeF = -sizeF;
      }
      world->dht.spikeF = sizeF;
      tracef("EVENT", "DHT22 sends one valid but wrong reading, %+.0f F off", sizeF);
    }
  }
}

static void seasonEvents() {
  size_t entryCount = 0;
  const ScheduleEntry *schedule = seasonSchedule(entryCount);
  if (at(0.25)) {
    // the settings in force at the start: the last entry before the start hour (or yesterday's last)
    const ScheduleEntry *current = &schedule[entryCount - 1];
    for (size_t index = 0; index < entryCount; index++) {
      if (schedule[index].hour <= world->config.startHour) {
        current = &schedule[index];
      }
    }
    applyScheduleEntry(*current);
    applyFanPlanAtStart();
  }
  double startHours = world->config.startHour;
  double fromHours = startHours + (double)world->stepFromUs / (double)US_PER_HOUR_SIM;
  double toHours = startHours + (double)world->stepToUs / (double)US_PER_HOUR_SIM;
  int64_t firstDay = (int64_t)floor(fromHours / 24.0);
  int64_t lastDay = (int64_t)floor(toHours / 24.0);
  for (int64_t dayIndex = firstDay; dayIndex <= lastDay; dayIndex++) {
    for (size_t index = 0; index < entryCount; index++) {
      if (at(minuteOfClock(dayIndex, schedule[index].hour))) {
        applyScheduleEntry(schedule[index]);
      }
    }
    runFanSchedule(dayIndex);
    if (world->config.randomFaults) {
      runRandomFaults(dayIndex);
    }
  }
}

static void seasonFinish() {
  scenarioExpect(world->total.heatStarts + world->total.coolStarts > 0, "the equipment ran (%u heat, %u cool starts)",
                 world->total.heatStarts, world->total.coolStarts);
}

// ==== the table ====

const ScenarioDef SCENARIOS[] = {
  { "startup", "power-up: relays open, EEPROM defaults, 5 min power-up delay, first heat call",
    SEASON_WINTER, 0.75, 5.0, 64.0, false, false, nullptr, startupEvents, startupFinish },
  { "api-selections", "every API command and value (valid and invalid), mode changes inside minimum on times, "
    "dead band, compressor off time, off, fan only, reset",
    SEASON_SPRING, 2.0, 6.0, 67.0, false, false, nullptr, apiSelectionsEvents, nullptr },
  { "fan-cool", "cooling with fan auto (G with Y1, 60 s run-on), fan on, fan only",
    SEASON_SUMMER, 4.0, 12.0, 77.0, false, false, nullptr, fanCoolEvents, fanCoolFinish },
  { "fan-heat", "heating with fan auto (G open, the furnace runs its blower), fan on with heat, fan only",
    SEASON_WINTER, 4.0, 6.0, 66.0, false, false, nullptr, fanHeatEvents, fanHeatFinish },
  { "sensor-missing", "DHT22 unplugged at power-up: startup lockout with every relay open (even fan on), the 25 % fallback once heat is set, cleared when "
    "plugged in", SEASON_WINTER, 2.0, 22.0, 65.0, false, false, sensorMissingPrepare, sensorMissingEvents, nullptr },
  { "sensor-dead-restart", "the DHT22 dies, then a power cut with heat saved: locked out, then the 25 % fallback 10 min after boot",
    SEASON_WINTER, 0.5, 6.0, 67.0, false, false, nullptr, sensorDeadRestartEvents, nullptr },
  { "sensor-intermittent", "8 % of DHT22 frames fail all day (cable noise): single failures never go stale",
    SEASON_WINTER, 24.0, 0.0, 68.0, false, true, sensorIntermittentPrepare, sensorIntermittentEvents, sensorIntermittentFinish },
  { "sensor-stale", "DHT22 drops out for 4 and 3 minutes while cooling: stale pauses cooling (with run-on), then back",
    SEASON_SUMMER, 3.0, 14.0, 78.0, false, false, nullptr, sensorStaleEvents, sensorStaleFinish },
  { "sensor-fail-heat", "DHT22 dead for 2 h in heat: stale stops the furnace, failed at 10 min starts a 25 % duty, cool refused, recovery",
    SEASON_WINTER, 4.0, 1.0, 67.0, false, false, nullptr, sensorFailHeatEvents, sensorFailHeatFinish },
  { "sensor-fail-cool", "DHT22 dies 1 min into a compressor run: held to its 5 min minimum, stopped with run-on, cool "
    "switched off and saved at 10 min, refused until it reads", SEASON_SUMMER, 2.0, 15.0, 80.0, false, false, nullptr, sensorFailCoolEvents, nullptr },
  { "sensor-bad-data", "each DHT22 failure in turn (data timeout, bad checksum, out of range, shorted line), then 15 min "
    "of garbage", SEASON_WINTER, 2.5, 3.0, 67.0, false, false, nullptr, sensorBadDataEvents, nullptr },
  { "sensor-spike", "valid but wrong single readings (+-20 and +40 F) are rejected: none starts or stops the furnace",
    SEASON_WINTER, 3.0, 2.0, 68.0, false, false, nullptr, sensorSpikeEvents, nullptr },
  { "run-limit-heat-frozen", "the reading freezes low in heat: 1 h runs with 30 min rests, the 25 % fallback after 3, normal once it moves",
    SEASON_WINTER, 5.5, 6.0, 67.0, false, false, runLimitHeatFrozenPrepare, runLimitHeatFrozenEvents, nullptr },
  { "run-limit-cool-frozen", "the reading freezes high in cool: 2 h runs with 30 min rests, cool off and saved after 3, a mode clears it",
    SEASON_SUMMER, 7.3, 12.0, 79.0, false, false, runLimitCoolFrozenPrepare, runLimitCoolFrozenEvents, nullptr },
  { "run-limit-lockout", "a furnace lockout: 1 h runs with 30 min rests and the alarm, never counted as a frozen reading",
    SEASON_WINTER, 5.0, 6.0, 67.0, false, false, nullptr, runLimitLockoutEvents, runLimitLockoutFinish },
  { "power-loss", "a 30 s power blip and a 2 h outage while heating: settings restored, power-up delay, recovery",
    SEASON_WINTER, 6.0, 4.0, 67.0, false, false, nullptr, powerLossEvents, powerLossFinish },
  { "power-blip-cool", "a 15 s power blip with the compressor running: it stays off 5 min after the restart",
    SEASON_SUMMER, 1.0, 14.0, 79.0, false, false, nullptr, powerBlipCoolEvents, nullptr },
  { "power-loss-unsaved", "power lost inside the 5 min EEPROM write limit: the last saved settings come back",
    SEASON_SPRING, 0.75, 9.0, 68.0, false, false, nullptr, powerLossUnsavedEvents, nullptr },
  { "eeprom-corrupt", "settings from another firmware build, then flash bit rot: defaults (off) both times",
    SEASON_WINTER, 0.5, 6.0, 66.0, false, false, eepromCorruptPrepare, eepromCorruptEvents, nullptr },
  { "eeprom-write-fail", "flash writes fail 3 times: wipe failure logged, saves retried every 5 min until one lands",
    SEASON_WINTER, 0.5, 6.0, 66.0, false, false, eepromWriteFailPrepare, eepromWriteFailEvents, nullptr },
  { "wifi-short", "a 14.5 min Wi-Fi outage: HVAC carries on, reconnect attempts every 2 min, no restart",
    SEASON_WINTER, 1.5, 7.0, 67.0, false, false, nullptr, wifiShortEvents, wifiShortFinish },
  { "wifi-restart", "a 90 min Wi-Fi outage: 10 attempts, then a restart once the furnace is off, repeatedly",
    SEASON_WINTER, 2.5, 5.0, 67.0, false, false, nullptr, wifiRestartEvents, wifiRestartFinish },
  { "wifi-restart-wait", "the access point returns while the furnace holds the restart up: retries continue, restart cancelled",
    SEASON_WINTER, 0.75, 5.0, 60.0, false, false, nullptr, wifiRestartWaitEvents, wifiRestartWaitFinish },
  { "wifi-startup", "the first join fails after a power cut: retries every 15 s for 5 min, then every 2 min",
    SEASON_WINTER, 1.0, 7.0, 67.0, false, false, nullptr, wifiStartupEvents, wifiStartupFinish },
  { "watchdog-core0", "core 0 (Wi-Fi/API) hangs: core 1 stops feeding the watchdog after 30 s, restart with relays open",
    SEASON_WINTER, 1.0, 6.0, 67.0, false, false, nullptr, watchdogCore0Events, watchdogCore0Finish },
  { "watchdog-loop", "core 0 hangs after every boot: safe mode after 3 restarts, the furnace runs, networking back after 1 h",
    SEASON_WINTER, 1.5, 6.0, 64.0, false, false, nullptr, watchdogLoopEvents, watchdogLoopFinish },
  { "watchdog-core1", "core 1 (HVAC) hangs with the compressor on: the watchdog restarts the Pico in 8 s",
    SEASON_SUMMER, 0.67, 14.0, 79.0, false, false, nullptr, watchdogCore1Events, watchdogCore1Finish },
  { "short-cycle-cool", "the setpoint flips 72/80 every minute for 2 h: compressor minimums and 4 starts an hour hold",
    SEASON_SUMMER, 3.0, 12.0, 76.0, false, false, nullptr, shortCycleCoolEvents, shortCycleCoolFinish },
  { "short-cycle-heat", "the setpoint flips 64/76 every minute for 2 h: furnace minimum and 6 starts an hour hold",
    SEASON_WINTER, 3.0, 2.0, 68.0, false, false, nullptr, shortCycleHeatEvents, shortCycleHeatFinish },
  { "extreme-cold", "two days of polar vortex (mean -12 F) in heat",
    SEASON_WINTER, 48.0, 0.0, 68.0, false, false, extremeColdPrepare, extremeColdEvents, nullptr },
  { "heat-wave", "two days of heat wave (afternoons over 105 F) in cool",
    SEASON_SUMMER, 48.0, 0.0, 76.0, false, false, heatWavePrepare, heatWaveEvents, nullptr },
  { "season", "--days of the --season with a household schedule through the API (setbacks, fan schedule; spring and "
    "fall switch heat/off/cool daily)", SEASON_WINTER, 0.0, 0.0, 68.0, true, true, nullptr, seasonEvents, seasonFinish },
  { "season-faults", "the season run plus random realistic faults every day (sensor dropouts and noise, Wi-Fi outages, "
    "power cuts, hangs, flash errors, wrong readings)", SEASON_WINTER, 0.0, 0.0, 68.0, true, false, nullptr, seasonEvents,
    seasonFinish },
};

const ScenarioDef *scenarioTable(size_t &count) {
  count = sizeof(SCENARIOS) / sizeof(SCENARIOS[0]);
  return SCENARIOS;
}
