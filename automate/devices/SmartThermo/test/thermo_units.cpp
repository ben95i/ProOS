// =====================================================================================
// test/thermo_units.cpp
// SmartThermo host test: unit tests of thermo.cpp's own functions. thermo.cpp is compiled here
// (not separately) so its internal functions, types and state are in reach; the scenarios link
// against this copy too.
//
// Covers: the pins and timing constants against the HVAC reference, the DHT22 decode (datasheet
// example, negative, limits, checksum), the DHT22 bit-banged read over the simulated line (good
// frames and every line fault), the plausibility and jump checks, the read schedule, the sensor
// state machine and its log lines, the run limits, the 25 % heat fallback phase, the switching temperatures, every start blocker, the starts per
// hour history, the final interlock, the relay switching order, the fan decision, the safety
// stops, the status text and the names.
// =====================================================================================
#include "../thermo.cpp"

#include <pico/mutex.h>
#include <stdarg.h>
#include <string>

#include "../display.h"
#include "sim_world.h"

extern recursive_mutex_t stateMutex;          // SmartThermo.ino
void runCommand(uint16_t command, uint16_t value); // SmartThermo.ino, the API routes
void logSettledBacklight();                        // SmartThermo.ino
extern bool backlightLogPending;                   // SmartThermo.ino
extern uint8_t backlightLoggedLevel;
extern uint64_t backlightLastChangeMs;
extern uint64_t backlightLastLogMs;

static uint32_t unitChecks = 0;
static uint32_t unitFailures = 0;

static void unitExpect(bool passed, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void unitExpect(bool passed, const char *format, ...) {
  char text[FAILURE_TEXT_LENGTH];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  unitChecks = unitChecks + 1;
  if (passed) {
    tracef("UNIT", "ok    %s", text);
    return;
  }
  unitFailures = unitFailures + 1;
  checkFail("unit", "%s", text);
}

static void unitSection(const char *name) {
  tracef("UNIT", "---- %s ----", name);
}

static void setUptimeMs(uint64_t uptimeMilliseconds) {
  world->nowUs = world->bootUs + uptimeMilliseconds * US_PER_MS_SIM;
}

static const uint64_t MINUTE_MS = 60000;

// Puts thermo.cpp's state and the settings back to a known start: idle, sensor ok at 70.0 F,
// heat and cool off since boot, mode off, fan auto, 70 F, booted at uptime 0.
static void resetThermoState() {
  relayState.heatW1 = false;
  relayState.coolY1 = false;
  relayState.fanG = false;
  heatOnSinceMs = 0;
  heatLastOffMs = 0;
  coolOnSinceMs = 0;
  coolLastOffMs = 0;
  coolRunOnAllowed = false;
  heatMinOnHoldLogged = false;
  coolMinOnHoldLogged = false;
  heatChangeReason = "";
  coolChangeReason = "";
  fanChangeReason = "";
  heatBlocker = BLOCKER_NONE;
  coolBlocker = BLOCKER_NONE;
  lastInterlockMessage = nullptr;
  interlockRefusals = 0;
  heatStarts = {};
  coolStarts = {};
  sensorState = SENSOR_STATE_OK;
  sensorHasReading = true;
  sensorTemperatureTenthsF = 700;
  sensorHumidityTenths = 450;
  sensorLastValidMs = 0;
  sensorNextReadMs = 0;
  sensorReadsValid = 0;
  sensorReadsFailed = 0;
  sensorConsecutiveFailures = 0;
  sensorLastResult = DHT_RESULT_OK;
  sensorJumpPending = false;
  responseTracking = false;
  noResponseAlarm = false;
  runLimitStopDue = false;
  runLimitResting = false;
  frozenStopCount = 0;
  frozenReadingActive = false;
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  settings.fanMode = (uint8_t)FAN_MODE_AUTO;
  settings.setpointF = 70;
  settings.coolShutOff = 0;
  settingsSavePending = false;
  bootMs = 0;
  setUptimeMs(0);
}

// Counts the captured firmware log lines holding the text.
static uint32_t loggedCount(const char *text) {
  drainFirmwareLog();
  uint32_t stored = world->firmwareLineCount;
  if (stored > FIRMWARE_LINE_CAPACITY) {
    stored = FIRMWARE_LINE_CAPACITY;
  }
  uint32_t found = 0;
  for (uint32_t index = 0; index < stored; index++) {
    if (strstr(world->firmwareLines[index].text, text) != nullptr) {
      found = found + 1;
    }
  }
  return found;
}

static bool logged(const char *text) {
  return loggedCount(text) > 0;
}

// ==== constants ====

static void testPinsAndConstants() {
  unitSection("pins and timing constants against the HVAC reference");
  unitExpect(RELAY_HEAT_W1_PIN == SIM_PIN_W1, "W1 relay on GP18 (got GP%u)", RELAY_HEAT_W1_PIN);
  unitExpect(RELAY_COOL_Y1_PIN == SIM_PIN_Y1, "Y1 relay on GP17 (got GP%u)", RELAY_COOL_Y1_PIN);
  unitExpect(RELAY_FAN_G_PIN == SIM_PIN_G, "G relay on GP16 (got GP%u)", RELAY_FAN_G_PIN);
  unitExpect(DHT22_DATA_PIN == SIM_PIN_DHT, "DHT22 data on GP22 (got GP%u)", DHT22_DATA_PIN);
  const uint8_t wifiAndAdcPins[] = { 23, 24, 25, 26, 27, 28, 29 };
  for (uint8_t reservedPin : wifiAndAdcPins) {
    bool clear = (RELAY_HEAT_W1_PIN != reservedPin) && (RELAY_COOL_Y1_PIN != reservedPin) && (RELAY_FAN_G_PIN != reservedPin) &&
                 (DHT22_DATA_PIN != reservedPin);
    unitExpect(clear, "no HVAC pin on GP%u (Wi-Fi chip / ADC)", reservedPin);
  }
  unitExpect(POWER_UP_DELAY_MS * US_PER_MS_SIM == SPEC_POWER_UP_DELAY_US, "power-up delay 5 min (%llu ms)",
             (unsigned long long)POWER_UP_DELAY_MS);
  unitExpect(COMPRESSOR_MIN_OFF_MS * US_PER_MS_SIM == SPEC_COMPRESSOR_MIN_OFF_US, "compressor minimum off 5 min");
  unitExpect(COMPRESSOR_MIN_ON_MS >= 3 * MINUTE_MS && COMPRESSOR_MIN_ON_MS <= 5 * MINUTE_MS, "compressor minimum on 3-5 min");
  unitExpect(FURNACE_MIN_ON_MS * US_PER_MS_SIM == SPEC_FURNACE_MIN_ON_US, "furnace minimum on 3 min");
  unitExpect(CHANGEOVER_DEAD_BAND_MS * US_PER_MS_SIM == SPEC_DEAD_BAND_US, "heat/cool dead band 5 min");
  unitExpect(COOL_FAN_RUN_ON_MS <= 90000 && COOL_FAN_RUN_ON_MS * US_PER_MS_SIM == SPEC_FAN_RUN_ON_US, "blower run-on 60 s (0-90 s)");
  unitExpect(COOL_STARTS_PER_HOUR_MAX >= 3 && COOL_STARTS_PER_HOUR_MAX <= 4, "cool starts per hour 3-4 (%u)", COOL_STARTS_PER_HOUR_MAX);
  unitExpect(HEAT_STARTS_PER_HOUR_MAX >= 5 && HEAT_STARTS_PER_HOUR_MAX <= 6, "heat starts per hour 5-6 (%u)", HEAT_STARTS_PER_HOUR_MAX);
  unitExpect(DHT_STARTUP_TIMEOUT_MS * US_PER_MS_SIM == SPEC_SENSOR_STARTUP_US, "startup lockout after 60 s without a reading");
  unitExpect(DHT_STALE_MS * US_PER_MS_SIM == SPEC_SENSOR_STALE_US, "stale after 60 s");
  unitExpect(DHT_FAILED_MS * US_PER_MS_SIM == SPEC_SENSOR_FAILED_US, "failed after 10 min without a reading");
  unitExpect(DHT_READ_INTERVAL_MS >= 2000, "DHT22 read at most every 2 s (%llu ms)", (unsigned long long)DHT_READ_INTERVAL_MS);
  unitExpect(HEAT_FALLBACK_ON_MS * 100 / HEAT_FALLBACK_PERIOD_MS == 25, "heat fallback 25 %% duty");
  unitExpect(TEMPERATURE_SWING_TENTHS_F == 10, "1 F swing centred on the setpoint");
}

// ==== time and text helpers (SmartThermo.ino) ====

static void testTimeAndTextHelpers() {
  unitSection("time and text helpers");
  unitExpect(msSince(10, 5) == 0, "msSince clamps a future start to 0");
  unitExpect(msSince(5, 10) == 5, "msSince(5, 10) = 5");
  unitExpect(!intervalElapsed(0, 100, 99) && intervalElapsed(0, 100, 100), "intervalElapsed at its boundary");
  unitExpect(msRemaining(0, 100, 30) == 70 && msRemaining(0, 100, 150) == 0, "msRemaining");
  unitExpect(msToSecondsRoundedUp(0) == 0 && msToSecondsRoundedUp(1) == 1 && msToSecondsRoundedUp(1000) == 1 &&
             msToSecondsRoundedUp(1001) == 2, "msToSecondsRoundedUp never shows a running wait as 0 s");
  unitExpect(msToSeconds(1999) == 1, "msToSeconds truncates");
  const int32_t tenths[] = { 713, -45, 0, -5, 1000 };
  const char *texts[] = { "71.3", "-4.5", "0.0", "-0.5", "100.0" };
  for (size_t index = 0; index < 5; index++) {
    char output[TEMPERATURE_TEXT_SIZE];
    formatTenths(output, sizeof(output), tenths[index]);
    unitExpect(strcmp(output, texts[index]) == 0, "formatTenths(%ld) = \"%s\" (got \"%s\")", (long)tenths[index], texts[index], output);
  }
}

// ==== DHT22 decode ====

struct DecodeCase {
  uint8_t bytes[5];
  DhtResult result;
  int32_t tenthsF;
  uint16_t humidityTenths;
  const char *description;
};

const DecodeCase DECODE_CASES[] = {
  { { 0x02, 0x8C, 0x01, 0x5F, 0xEE }, DHT_RESULT_OK, 951, 652, "datasheet example: 65.2 %, 35.1 C = 95.1 F (95.18 truncated)" },
  { { 0x01, 0x90, 0x00, 0x00, 0x91 }, DHT_RESULT_OK, 320, 400, "0.0 C = 32.0 F" },
  { { 0x01, 0x90, 0x80, 0x00, 0x11 }, DHT_RESULT_OK, 320, 400, "-0.0 C = 32.0 F" },
  { { 0x01, 0x90, 0x01, 0x00, 0x92 }, DHT_RESULT_OK, 780, 400, "25.6 C = 78.0 F (78.08 truncated)" },
  { { 0x01, 0xF4, 0x80, 0x65, 0xDA }, DHT_RESULT_OK, 139, 500, "-10.1 C = 13.9 F (13.82 rounded toward zero)" },
  { { 0x01, 0xF4, 0x81, 0x90, 0x06 }, DHT_RESULT_OK, -400, 500, "-40.0 C = -40.0 F, the bottom of the range" },
  { { 0x01, 0xF4, 0x81, 0x91, 0x07 }, DHT_RESULT_OUT_OF_RANGE, 0, 0, "-40.1 C is out of range" },
  { { 0x01, 0xF4, 0x03, 0x20, 0x18 }, DHT_RESULT_OK, 1760, 500, "80.0 C = 176.0 F, the top of the range" },
  { { 0x01, 0xF4, 0x03, 0x21, 0x19 }, DHT_RESULT_OUT_OF_RANGE, 0, 0, "80.1 C is out of range" },
  { { 0x03, 0xE8, 0x00, 0xFA, 0xE5 }, DHT_RESULT_OK, 770, 1000, "100.0 % humidity is in range" },
  { { 0x03, 0xE9, 0x00, 0xFA, 0xE6 }, DHT_RESULT_OUT_OF_RANGE, 0, 0, "100.1 % humidity is out of range" },
  { { 0x03, 0x20, 0x00, 0xFA, 0x1D }, DHT_RESULT_OK, 770, 800, "checksum is the low byte of the sum (0x11D)" },
  { { 0x02, 0x8C, 0x01, 0x5F, 0xEF }, DHT_RESULT_BAD_CHECKSUM, 0, 0, "checksum off by one" },
  { { 0x02, 0x8D, 0x01, 0x5F, 0xEE }, DHT_RESULT_BAD_CHECKSUM, 0, 0, "one data bit flipped" },
  { { 0xFF, 0xFF, 0x7F, 0xFF, 0x7C }, DHT_RESULT_OUT_OF_RANGE, 0, 0, "garbage with a valid checksum" },
};

static void testDhtDecode() {
  unitSection("DHT22 decode");
  for (const DecodeCase &testCase : DECODE_CASES) {
    const int32_t untouchedTenthsF = -9999;
    const uint16_t untouchedHumidity = 9999;
    int32_t tenthsF = untouchedTenthsF;
    uint16_t humidityTenths = untouchedHumidity;
    DhtResult result = decodeDht22Bytes(testCase.bytes, tenthsF, humidityTenths);
    if (testCase.result == DHT_RESULT_OK) {
      bool decoded = (result == DHT_RESULT_OK) && (tenthsF == testCase.tenthsF) && (humidityTenths == testCase.humidityTenths);
      unitExpect(decoded, "%s (got %s, %ld tenths F, %u tenths %%)", testCase.description, dhtResultName(result), (long)tenthsF,
                 humidityTenths);
    } else {
      bool rejected = (result == testCase.result) && (tenthsF == untouchedTenthsF) && (humidityTenths == untouchedHumidity);
      unitExpect(rejected, "%s: %s, outputs untouched (got %s)", testCase.description, dhtResultName(testCase.result),
                 dhtResultName(result));
    }
  }
}

// ==== DHT22 read over the simulated data line ====

static void testDhtLine() {
  unitSection("DHT22 read over the data line (start signal, 80 us answer, 40 bits by pulse length)");
  struct LineCase {
    uint8_t frame[5];
    SensorFault fault;
    DhtResult expected;
    const char *description;
  };
  const LineCase cases[] = {
    { { 0x02, 0x8C, 0x01, 0x5F, 0xEE }, SENSOR_FAULT_NONE, DHT_RESULT_OK, "datasheet frame read bit for bit" },
    { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFC }, SENSOR_FAULT_NONE, DHT_RESULT_OK, "all ones" },
    { { 0x00, 0x00, 0x00, 0x00, 0x00 }, SENSOR_FAULT_NONE, DHT_RESULT_OK, "all zeros" },
    { { 0xA5, 0x5A, 0x81, 0x18, 0x98 }, SENSOR_FAULT_NONE, DHT_RESULT_OK, "alternating bits" },
    { { 0x02, 0x8C, 0x01, 0x5F, 0xEE }, SENSOR_FAULT_UNPLUGGED, DHT_RESULT_NO_RESPONSE, "unplugged: no response" },
    { { 0x02, 0x8C, 0x01, 0x5F, 0xEE }, SENSOR_FAULT_STUCK_LOW, DHT_RESULT_NO_RESPONSE, "line shorted low: no response" },
    { { 0x02, 0x8C, 0x01, 0x5F, 0xEE }, SENSOR_FAULT_DATA_TIMEOUT, DHT_RESULT_DATA_TIMEOUT, "stops part way: data timeout" },
  };
  for (const LineCase &testCase : cases) {
    for (int repeat = 0; repeat < 3; repeat++) {
      uint8_t bytes[5] = { 0, 0, 0, 0, 0 };
      simForceNextDhtFrame(testCase.frame, testCase.fault);
      DhtResult result = readDht22Bytes(bytes);
      bool resultMatches = (result == testCase.expected);
      bool bytesMatch = (testCase.expected != DHT_RESULT_OK) || (memcmp(bytes, testCase.frame, 5) == 0);
      unitExpect(resultMatches && bytesMatch, "%s (got %s, %02X %02X %02X %02X %02X)", testCase.description, dhtResultName(result),
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
      unitExpect(simInterruptsEnabled(), "interrupts back on after the read");
      world->nowUs = world->nowUs + 5 * US_PER_SECOND_SIM;
    }
  }
  unitExpect(world->checks.maxInterruptsOffUs <= SPEC_MAX_INTERRUPTS_OFF_US, "interrupts held off at most %.2f ms",
             (double)world->checks.maxInterruptsOffUs / 1000.0);
}

// ==== believable readings ====

static void testBelievableReadings() {
  unitSection("believable readings");
  resetThermoState();
  uint8_t zeroFrame[5] = { 0, 0, 0, 0, 0 };
  int32_t tenthsF = 0;
  uint16_t humidityTenths = 0;
  unitExpect(decodeDht22Bytes(zeroFrame, tenthsF, humidityTenths) == DHT_RESULT_OK, "an all-zero frame passes its checksum");
  sensorHasReading = false;
  unitExpect(checkReadingBelievable(tenthsF, humidityTenths) == DHT_RESULT_IMPLAUSIBLE, "but 32.0 F, 0.0 %% is rejected");
  unitExpect(checkReadingBelievable(199, 450) == DHT_RESULT_IMPLAUSIBLE, "19.9 F is rejected");
  unitExpect(checkReadingBelievable(1201, 450) == DHT_RESULT_IMPLAUSIBLE, "120.1 F is rejected");
  unitExpect(checkReadingBelievable(700, 9) == DHT_RESULT_IMPLAUSIBLE, "0.9 %% humidity is rejected");
  unitExpect(checkReadingBelievable(200, 10) == DHT_RESULT_OK, "20.0 F, 1.0 %% is accepted, the first reading since boot");
  unitExpect(checkReadingBelievable(1200, 450) == DHT_RESULT_OK, "120.0 F is accepted, the first reading since boot");
  sensorHasReading = true;
  sensorTemperatureTenthsF = 700;
  unitExpect(checkReadingBelievable(750, 450) == DHT_RESULT_OK, "exactly 5 F up is accepted");
  unitExpect(checkReadingBelievable(650, 450) == DHT_RESULT_OK, "exactly 5 F down is accepted");
  unitExpect(checkReadingBelievable(503, 450) == DHT_RESULT_UNCONFIRMED_JUMP, "20 F low waits to be repeated");
  unitExpect(checkReadingBelievable(701, 450) == DHT_RESULT_OK, "the next normal reading is accepted");
  unitExpect(!sensorJumpPending, "and the jump is forgotten");
  unitExpect(checkReadingBelievable(503, 450) == DHT_RESULT_UNCONFIRMED_JUMP, "another single jump waits again");
  unitExpect(checkReadingBelievable(510, 450) == DHT_RESULT_OK, "a jump that repeats is real and accepted");
  sensorJumpPending = false;
  unitExpect(checkReadingBelievable(900, 450) == DHT_RESULT_UNCONFIRMED_JUMP, "up by 20 F waits");
  unitExpect(checkReadingBelievable(500, 450) == DHT_RESULT_UNCONFIRMED_JUMP, "a different jump replaces it and waits");
  unitExpect(checkReadingBelievable(495, 450) == DHT_RESULT_OK, "and is accepted when repeated");
  resetThermoState();
  world->dht.fault = SENSOR_FAULT_NONE;
  initSensor();
  sensorHasReading = true;
  sensorTemperatureTenthsF = 300; // far from the simulated room's 70 F
  setUptimeMs(2000);
  readSensorWhenDue();
  unitExpect(sensorLastResult == DHT_RESULT_UNCONFIRMED_JUMP && sensorTemperatureTenthsF == 300 && sensorLastValidMs == 0,
             "a rejected reading changes nothing and doesn't count as valid");
  unitExpect(sensorReadsFailed == 1 && logged("DHT22 read failed: unconfirmed jump"), "it is counted and logged as a failed read");
  setUptimeMs(7000);
  readSensorWhenDue();
  unitExpect(sensorLastResult == DHT_RESULT_OK && sensorTemperatureTenthsF != 300 && sensorLastValidMs == 7000, "accepted when repeated");
}

// ==== read schedule and failure logging ====

static void testReadSchedule() {
  unitSection("DHT22 read schedule");
  resetThermoState();
  sensorHasReading = false;
  sensorState = SENSOR_STATE_STARTING;
  world->dht.fault = SENSOR_FAULT_NONE;
  initSensor();
  uint64_t readsBefore = world->dht.reads;
  setUptimeMs(1999);
  readSensorWhenDue();
  unitExpect(world->dht.reads == readsBefore, "no read before the sensor's 2 s power-up time");
  setUptimeMs(2000);
  readSensorWhenDue();
  unitExpect(world->dht.reads == readsBefore + 1, "first read 2 s after boot");
  bool plausible = sensorHasReading && sensorTemperatureTenthsF >= 690 && sensorTemperatureTenthsF <= 710;
  unitExpect(plausible, "the reading is the room's 70 F (got %ld tenths)", (long)sensorTemperatureTenthsF);
  setUptimeMs(6999);
  readSensorWhenDue();
  unitExpect(world->dht.reads == readsBefore + 1, "no read within 5 s of the last");
  setUptimeMs(7000);
  readSensorWhenDue();
  unitExpect(world->dht.reads == readsBefore + 2, "next read 5 s later");
  world->dht.fault = SENSOR_FAULT_UNPLUGGED;
  setUptimeMs(12000);
  readSensorWhenDue();
  setUptimeMs(17000);
  readSensorWhenDue();
  setUptimeMs(22000);
  readSensorWhenDue();
  unitExpect(sensorReadsFailed == 3 && sensorConsecutiveFailures == 3 && sensorLastResult == DHT_RESULT_NO_RESPONSE,
             "failed reads counted (%lu failed, %lu in a row)", (unsigned long)sensorReadsFailed,
             (unsigned long)sensorConsecutiveFailures);
  unitExpect(loggedCount("DHT22 read failed: no response") == 1, "a run of failures is logged once");
  unitExpect(sensorLastValidMs == 7000, "the last good reading's time is kept");
  world->dht.fault = SENSOR_FAULT_NONE;
  setUptimeMs(27000);
  readSensorWhenDue();
  unitExpect(sensorConsecutiveFailures == 0 && sensorLastValidMs == 27000, "a good read clears the failure run");
}

// ==== sensor state machine ====

// Rounds the sensor's tenths for the display, returning kept when there is no usable reading.
static int16_t wholeTemperatureOf(int32_t temperatureTenthsF, int16_t kept) {
  sensorTemperatureTenthsF = temperatureTenthsF;
  int16_t wholeDegreesF = kept;
  readWholeTemperatureF(wholeDegreesF);
  return wholeDegreesF;
}

static void testDisplayReadings() {
  unitSection("display readings");
  resetThermoState();
  sensorState = SENSOR_STATE_OK;
  unitExpect(wholeTemperatureOf(713, 0) == 71, "71.3 F shows 71");
  unitExpect(wholeTemperatureOf(715, 0) == 72, "71.5 F shows 72");
  unitExpect(wholeTemperatureOf(-45, 0) == -5, "-4.5 F shows -5");
  unitExpect(wholeTemperatureOf(-44, 0) == -4, "-4.4 F shows -4");
  sensorState = SENSOR_STATE_STALE;
  unitExpect(wholeTemperatureOf(713, 0) == 71, "a stale reading still shows");
  int16_t wholeDegreesF = 0;
  sensorState = SENSOR_STATE_FAILED;
  unitExpect(!readWholeTemperatureF(wholeDegreesF), "no temperature once the sensor has failed");
  sensorState = SENSOR_STATE_LOCKOUT;
  unitExpect(!readWholeTemperatureF(wholeDegreesF), "no temperature in the startup lockout");
  sensorReadsValid = 3;
  sensorReadsFailed = 2;
  unitExpect(sensorSampleCount() == 5, "valid and failed reads both count as samples");
}

// Puts the backlight's log state back to where boot leaves it.
static void resetBacklightLog() {
  backlightLogPending = false;
  backlightLoggedLevel = DISPLAY_BRIGHTNESS_DEFAULT;
  backlightLastChangeMs = 0;
  backlightLastLogMs = 0;
}

// Sends one backlight level at the given uptime, then runs the log check as core 0's loop would.
static void sendBacklightAt(uint64_t uptimeMilliseconds, uint16_t level) {
  setUptimeMs(uptimeMilliseconds);
  runCommand(6, level);
  logSettledBacklight();
}

// Runs the log check at the given uptime, as core 0's loop would.
static void checkBacklightLogAt(uint64_t uptimeMilliseconds) {
  setUptimeMs(uptimeMilliseconds);
  logSettledBacklight();
}

static void testBacklightLog() {
  unitSection("backlight log");
  resetThermoState();
  resetBacklightLog();
  const char *anyLevel = "default backlight set to";
  uint32_t linesBefore = loggedCount(anyLevel);
  // a 10 s drag, one level every 250 ms as the website's slider sends them
  uint64_t dragStartMs = 100000;
  const uint64_t SLIDER_SEND_MS = 250;
  const uint16_t DRAG_LEVELS = 40;
  for (uint16_t step = 0; step < DRAG_LEVELS; step++) {
    uint64_t stepOffsetMs = step * SLIDER_SEND_MS;
    uint16_t level = 10 + step;
    sendBacklightAt(dragStartMs + stepOffsetMs, level);
  }
  uint64_t lastLevelMs = dragStartMs + ((DRAG_LEVELS - 1) * SLIDER_SEND_MS);
  unitExpect(loggedCount(anyLevel) == linesBefore, "nothing is logged during a drag");
  checkBacklightLogAt(lastLevelMs + 1999);
  unitExpect(loggedCount(anyLevel) == linesBefore, "nothing is logged before the level settles");
  checkBacklightLogAt(lastLevelMs + 2000);
  unitExpect(loggedCount(anyLevel) == linesBefore + 1 && logged("default backlight set to 49"),
             "one entry with the final level once it has settled");
  uint64_t firstLogMs = lastLevelMs + 2000;
  // a second change soon after waits out the rest of the minute
  sendBacklightAt(firstLogMs + 10000, 60);
  checkBacklightLogAt(firstLogMs + MINUTE_MS - 1);
  unitExpect(loggedCount(anyLevel) == linesBefore + 1, "no second entry within the minute");
  checkBacklightLogAt(firstLogMs + MINUTE_MS);
  unitExpect(loggedCount(anyLevel) == linesBefore + 2 && logged("default backlight set to 60"),
             "the settled level is logged when the minute is up");
  // a level dragged away and back writes nothing
  uint64_t secondLogMs = firstLogMs + MINUTE_MS;
  sendBacklightAt(secondLogMs + MINUTE_MS, 70);
  sendBacklightAt(secondLogMs + MINUTE_MS + 250, 60);
  checkBacklightLogAt(secondLogMs + MINUTE_MS + 5000);
  unitExpect(loggedCount(anyLevel) == linesBefore + 2 && !backlightLogPending,
             "a level dragged back to the one logged writes nothing");
  runCommand(6, DISPLAY_BRIGHTNESS_DEFAULT);
  resetBacklightLog();
  setUptimeMs(0);
}

static void testSensorStates() {
  unitSection("sensor states");
  resetThermoState();
  sensorHasReading = false;
  unitExpect(evaluateSensorState(59999) == SENSOR_STATE_STARTING, "starting until 60 s");
  unitExpect(evaluateSensorState(60000) == SENSOR_STATE_LOCKOUT, "startup lockout at 60 s without a reading");
  unitExpect(evaluateSensorState(600000) == SENSOR_STATE_LOCKOUT, "mode off: still locked out at 10 min");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  unitExpect(evaluateSensorState(600000) == SENSOR_STATE_LOCKOUT, "mode cool: still locked out at 10 min");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  unitExpect(evaluateSensorState(599999) == SENSOR_STATE_LOCKOUT, "mode heat: locked out until 10 min");
  unitExpect(evaluateSensorState(600000) == SENSOR_STATE_FAILED, "mode heat: failed at 10 min, so the fallback runs");
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  sensorHasReading = true;
  sensorLastValidMs = 100000;
  unitExpect(evaluateSensorState(159999) == SENSOR_STATE_OK, "ok within 60 s of a reading");
  unitExpect(evaluateSensorState(160000) == SENSOR_STATE_STALE, "stale at 60 s");
  unitExpect(evaluateSensorState(699999) == SENSOR_STATE_STALE, "still stale at 9:59.999");
  unitExpect(evaluateSensorState(700000) == SENSOR_STATE_FAILED, "failed at 10 min");

  resetThermoState();
  sensorHasReading = false;
  sensorState = SENSOR_STATE_STARTING;
  sensorLastResult = DHT_RESULT_NO_RESPONSE;
  setUptimeMs(60000);
  updateSensorState(60000);
  unitExpect(sensorState == SENSOR_STATE_LOCKOUT && logged("ERROR DHT22 not readable at startup (no response): HVAC locked out"),
             "lockout logged with the read result");
  unitExpect(relayHoldReason() == BLOCKER_SENSOR_LOCKOUT, "the lockout holds every relay");
  sensorHasReading = true;
  sensorLastValidMs = 61000;
  sensorLastResult = DHT_RESULT_OK;
  updateSensorState(61000);
  unitExpect(sensorState == SENSOR_STATE_OK && logged("DHT22 reading: 70.0 F, 45.0 %, startup lockout cleared"), "lockout cleared");
  unitExpect(relayHoldReason() == BLOCKER_NONE, "no hold once the sensor reads");
  sensorLastResult = DHT_RESULT_BAD_CHECKSUM;
  updateSensorState(121000);
  unitExpect(sensorState == SENSOR_STATE_STALE && logged("ERROR DHT22 no reading for 60s (bad checksum), heat/cool paused"),
             "stale logged");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  settingsSavePending = false;
  updateSensorState(661000);
  unitExpect(sensorState == SENSOR_STATE_FAILED && logged("ERROR DHT22 failed, no reading for 600s (bad checksum)"), "failed logged");
  unitExpect(settings.hvacMode == HVAC_MODE_OFF && settingsSavePending && logged("ERROR DHT22 failed: cool mode switched off"),
             "cool mode switched off and marked for saving");
  unitExpect(settings.coolShutOff == 1, "the shut-off is recorded in the settings");
  sensorState = SENSOR_STATE_STALE;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  updateSensorState(661000);
  unitExpect(settings.hvacMode == HVAC_MODE_HEAT && logged("ERROR DHT22 failed: heat running 25% (300s every 1200s)"),
             "heat stays on, on the 25 %% fallback");
  sensorLastValidMs = 700000;
  updateSensorState(700000);
  unitExpect(sensorState == SENSOR_STATE_OK && logged("DHT22 reading again: 70.0 F, 45.0 %"), "recovery logged");
}

// ==== run limits ====

// Runs one stage for its whole no-response window with the reading unchanged, ending in the stop,
// then the rest, as updateThermostat would. Returns the time the rest ended.
static uint64_t runStageToNoResponseStop(bool heating, uint64_t startMs) {
  uint64_t windowMs = heating ? HEAT_RESPONSE_WINDOW_MS : COOL_RESPONSE_WINDOW_MS;
  relayState.heatW1 = heating;
  relayState.coolY1 = !heating;
  relayState.fanG = !heating;
  heatOnSinceMs = startMs;
  coolOnSinceMs = startMs;
  updateRunLimits(startMs);
  uint64_t stopMs = startMs + windowMs;
  updateRunLimits(stopMs);
  relayState.heatW1 = false;
  relayState.coolY1 = false;
  relayState.fanG = false;
  uint64_t restEndMs = stopMs + RUN_LIMIT_REST_MS;
  updateRunLimits(restEndMs);
  return restEndMs;
}

static void testRunLimits() {
  unitSection("run limits");
  resetThermoState();
  startHttpServer(runCommand);
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  const uint64_t startMs = 1000000;
  const uint64_t heatStopMs = startMs + HEAT_RESPONSE_WINDOW_MS;
  relayState.heatW1 = true;
  heatOnSinceMs = startMs;
  sensorTemperatureTenthsF = 650;
  updateRunLimits(startMs);
  updateRunLimits(heatStopMs - 1);
  unitExpect(!runLimitStopDue && keepHeating(heatStopMs - 1, BLOCKER_NONE), "W1 runs on inside its 1 h window");
  sensorTemperatureTenthsF = 659;
  updateRunLimits(heatStopMs);
  unitExpect(runLimitStopDue && runLimitResting && noResponseAlarm && logged("ERROR heat: no temperature response for 3600s, resting 1800s"),
             "0.9 F in 1 h stops W1");
  unitExpect(!keepHeating(heatStopMs, BLOCKER_NONE) && strcmp(heatChangeReason, "no temperature response") == 0, "W1 opens this pass");
  relayState.heatW1 = false;
  updateRunLimits(heatStopMs + 1000);
  unitExpect(!runLimitStopDue, "the stop lasts one pass");
  unitExpect(heatStartBlocker(heatStopMs + 1000, BLOCKER_NONE) == BLOCKER_RUN_LIMIT_REST, "the rest holds a heat start");
  unitExpect(coolStartBlocker(heatStopMs + 1000, BLOCKER_NONE) == BLOCKER_RUN_LIMIT_REST, "and a cool start");
  unitExpect(startBlockerRemainingMs(BLOCKER_RUN_LIMIT_REST, true, heatStopMs + 1000) == RUN_LIMIT_REST_MS - 1000, "the rest's time left");
  setUptimeMs(heatStopMs + 1000);
  std::string status = simRunCommand(10, 0);
  unitExpect(status.find("heat: no temperature response in its last run") != std::string::npos, "the alarm shows in the status");
  unitExpect(status.find("heat stopped at a run limit: resting 1799s more") != std::string::npos, "the rest shows in the status");
  updateRunLimits(heatStopMs + RUN_LIMIT_REST_MS - 1);
  unitExpect(runLimitResting, "still resting until 30 min");
  updateRunLimits(heatStopMs + RUN_LIMIT_REST_MS);
  unitExpect(!runLimitResting && noResponseAlarm, "the rest ends at 30 min, and the alarm stays");
  unitExpect(heatStartBlocker(heatStopMs + RUN_LIMIT_REST_MS, BLOCKER_NONE) == BLOCKER_NONE, "heat may start again");
  uint64_t retryMs = heatStopMs + RUN_LIMIT_REST_MS;
  relayState.heatW1 = true;
  heatOnSinceMs = retryMs;
  updateRunLimits(retryMs);
  sensorTemperatureTenthsF = 669;
  updateRunLimits(retryMs + 1000);
  unitExpect(!noResponseAlarm, "1.0 F warmer in a later run clears the alarm");

  // the hard cap holds even while the room improves
  resetThermoState();
  relayState.heatW1 = true;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  heatOnSinceMs = startMs;
  sensorTemperatureTenthsF = 650;
  updateRunLimits(startMs);
  sensorTemperatureTenthsF = 660;
  updateRunLimits(startMs + HEAT_MAX_RUN_MS - 1000);
  sensorTemperatureTenthsF = 670;
  updateRunLimits(startMs + HEAT_MAX_RUN_MS - 1);
  unitExpect(!runLimitStopDue, "W1 runs on just under 3 h while the room improves");
  updateRunLimits(startMs + HEAT_MAX_RUN_MS);
  unitExpect(runLimitStopDue && logged("ERROR heat: run limit of 10800s reached, resting 1800s") && strcmp(runLimitStopReason, "run limit") == 0, "W1 stops at 3 h");
  unitExpect(!noResponseAlarm && frozenStopCount == 0, "a hard cap isn't a no-response stop");
  resetThermoState();
  relayState.coolY1 = true;
  relayState.fanG = true;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  coolOnSinceMs = startMs;
  sensorTemperatureTenthsF = 900;
  updateRunLimits(startMs);
  sensorTemperatureTenthsF = 880;
  updateRunLimits(startMs + COOL_MAX_RUN_MS - 1);
  unitExpect(!runLimitStopDue, "Y1 runs on just under 6 h while the room improves");
  updateRunLimits(startMs + COOL_MAX_RUN_MS);
  unitExpect(runLimitStopDue && logged("ERROR cool: run limit of 21600s reached, resting 1800s"), "Y1 stops at 6 h");
  unitExpect(!keepCooling(startMs + COOL_MAX_RUN_MS, BLOCKER_NONE) && coolRunOnAllowed, "Y1 opens this pass, and G runs on");
  resetThermoState();
  relayState.heatW1 = true;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  heatOnSinceMs = startMs;
  sensorState = SENSOR_STATE_STALE;
  updateRunLimits(startMs + HEAT_RESPONSE_WINDOW_MS);
  unitExpect(!runLimitStopDue && !responseTracking, "only fresh readings count towards no response");

  // a frozen reading in heat: the third stop in a row ends in the 25 % fallback
  resetThermoState();
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorTemperatureTenthsF = 600;
  uint64_t nextStartMs = runStageToNoResponseStop(true, startMs);
  unitExpect(frozenStopCount == 1 && !frozenReadingActive, "one stop counted");
  nextStartMs = runStageToNoResponseStop(true, nextStartMs);
  unitExpect(frozenStopCount == 2 && !frozenReadingActive, "two stops counted");
  uint64_t thirdStopMs = nextStartMs + HEAT_RESPONSE_WINDOW_MS;
  runStageToNoResponseStop(true, nextStartMs);
  unitExpect(frozenReadingActive && logged("ERROR reading frozen: heat running 25% until it moves"), "the third stop drops heat to 25 %%");
  unitExpect(heatFallbackActive(), "the fallback is active");
  uint64_t fallbackStartMs = thirdStopMs + RUN_LIMIT_REST_MS;
  unitExpect(heatFallbackWantsOn(fallbackStartMs) && !heatFallbackWantsOn(fallbackStartMs + HEAT_FALLBACK_ON_MS), "5 min on from the rest's end");
  sensorState = SENSOR_STATE_STALE;
  unitExpect(!heatFallbackActive(), "a stale sensor's rules take over");
  sensorState = SENSOR_STATE_OK;
  sensorTemperatureTenthsF = 610;
  updateRunLimits(fallbackStartMs + 1000);
  unitExpect(!frozenReadingActive && !heatFallbackActive() && logged("reading moving again: heat back to normal control"), "1.0 F of movement ends it");

  // the reading moving at any point resets the count
  resetThermoState();
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorTemperatureTenthsF = 600;
  nextStartMs = runStageToNoResponseStop(true, startMs);
  nextStartMs = runStageToNoResponseStop(true, nextStartMs);
  sensorTemperatureTenthsF = 590;
  updateRunLimits(nextStartMs + 1000);
  unitExpect(frozenStopCount == 0, "the room drifting 1.0 F proves the sensor works, so the count resets");
  nextStartMs = runStageToNoResponseStop(true, nextStartMs + 2000);
  unitExpect(frozenStopCount == 1 && !frozenReadingActive, "and it starts again from one");

  // a frozen reading in cool: the third stop switches cool off, saved, with the error
  resetThermoState();
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  sensorTemperatureTenthsF = 850;
  nextStartMs = runStageToNoResponseStop(false, startMs);
  nextStartMs = runStageToNoResponseStop(false, nextStartMs);
  unitExpect(settings.hvacMode == HVAC_MODE_COOL, "cool stays on through two stops");
  runStageToNoResponseStop(false, nextStartMs);
  unitExpect(settings.hvacMode == HVAC_MODE_OFF && settings.coolShutOff == 1 && settingsSavePending, "the third stop switches cool off and saves it");
  unitExpect(logged("ERROR reading frozen: cool mode switched off"), "logged with its cause");

  // setting a mode clears everything in force
  resetThermoState();
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorTemperatureTenthsF = 600;
  nextStartMs = runStageToNoResponseStop(true, startMs);
  nextStartMs = runStageToNoResponseStop(true, nextStartMs);
  runStageToNoResponseStop(true, nextStartMs);
  unitExpect(frozenReadingActive, "frozen before the mode is set");
  runLimitResting = true;
  noResponseAlarm = true;
  frozenStopCount = 1;
  simRunCommand(4, HVAC_MODE_HEAT);
  unitExpect(!runLimitResting && !frozenReadingActive && !noResponseAlarm && frozenStopCount == 0, "setting a mode clears the run limits");
  resetThermoState();
}

// ==== 25 % heat fallback ====

static void testHeatFallback() {
  unitSection("25 % heat fallback");
  resetThermoState();
  sensorLastValidMs = 0; // failed since 600000
  struct PhaseCase {
    uint64_t nowMs;
    bool on;
  };
  const PhaseCase phases[] = { { 600000, true }, { 899999, true }, { 900000, false }, { 1799999, false },
                               { 1800000, true }, { 2099999, true }, { 2100000, false }, { 300000, true } };
  for (const PhaseCase &phase : phases) {
    unitExpect(heatFallbackWantsOn(phase.nowMs) == phase.on, "fallback at %llu ms is %s", (unsigned long long)phase.nowMs,
               phase.on ? "on" : "off");
  }
  uint64_t onSeconds = 0;
  uint64_t totalSeconds = 0;
  for (uint64_t nowMs = 600000; nowMs < 600000 + 4 * HEAT_FALLBACK_PERIOD_MS; nowMs += 1000) {
    totalSeconds = totalSeconds + 1;
    if (heatFallbackWantsOn(nowMs)) {
      onSeconds = onSeconds + 1;
    }
  }
  unitExpect(onSeconds * 4 == totalSeconds, "exactly 25 %% on over 80 min (%llu of %llu s)", (unsigned long long)onSeconds,
             (unsigned long long)totalSeconds);
  sensorState = SENSOR_STATE_FAILED;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  unitExpect(heatFallbackActive(), "fallback active: failed sensor in heat");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  unitExpect(!heatFallbackActive(), "no fallback in cool");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorState = SENSOR_STATE_STALE;
  unitExpect(!heatFallbackActive(), "no fallback while only stale");
}

// ==== switching temperatures and start decisions ====

static void testThresholds() {
  unitSection("switching temperatures (1 F swing)");
  resetThermoState();
  const uint16_t setpoints[] = { 50, 70, 80 };
  for (uint16_t setpointF : setpoints) {
    settings.setpointF = setpointF;
    int32_t setpointTenths = setpointF * 10;
    bool thresholds = (heatStartTenthsF() == setpointTenths - 5) && (heatStopTenthsF() == setpointTenths + 5) &&
                      (coolStartTenthsF() == setpointTenths + 5) && (coolStopTenthsF() == setpointTenths - 5);
    unitExpect(thresholds, "at %u F heat runs %ld-%ld, cool %ld-%ld tenths", setpointF, (long)heatStartTenthsF(),
               (long)heatStopTenthsF(), (long)coolStopTenthsF(), (long)coolStartTenthsF());
  }
  uint64_t nowMs = 10 * MINUTE_MS;
  setUptimeMs(nowMs);
  settings.setpointF = 70;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorTemperatureTenthsF = 695;
  unitExpect(startHeating(nowMs, BLOCKER_NONE), "heat starts at 69.5 F");
  sensorTemperatureTenthsF = 696;
  unitExpect(!startHeating(nowMs, BLOCKER_NONE), "no heat at 69.6 F");
  relayState.heatW1 = true;
  heatOnSinceMs = 0;
  sensorTemperatureTenthsF = 704;
  unitExpect(keepHeating(nowMs, BLOCKER_NONE), "heat continues at 70.4 F");
  sensorTemperatureTenthsF = 705;
  unitExpect(!keepHeating(nowMs, BLOCKER_NONE) && strcmp(heatChangeReason, "setpoint reached") == 0, "heat stops at 70.5 F");
  heatOnSinceMs = nowMs - 60000;
  unitExpect(keepHeating(nowMs, BLOCKER_NONE), "heat held at 70.5 F inside the furnace minimum on time");
  unitExpect(logged("heat setpoint reached, held for furnace minimum on time, 120s left"), "the hold is logged");

  resetThermoState();
  setUptimeMs(nowMs);
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  sensorTemperatureTenthsF = 705;
  unitExpect(startCooling(nowMs, BLOCKER_NONE), "cool starts at 70.5 F");
  sensorTemperatureTenthsF = 704;
  unitExpect(!startCooling(nowMs, BLOCKER_NONE), "no cool at 70.4 F");
  relayState.coolY1 = true;
  relayState.fanG = true;
  coolOnSinceMs = 0;
  sensorTemperatureTenthsF = 696;
  unitExpect(keepCooling(nowMs, BLOCKER_NONE), "cool continues at 69.6 F");
  sensorTemperatureTenthsF = 695;
  unitExpect(!keepCooling(nowMs, BLOCKER_NONE) && coolRunOnAllowed, "cool stops at 69.5 F and the blower may run on");
}

// ==== start blockers ====

static void testBlockers() {
  unitSection("start blockers");
  resetThermoState();
  unitExpect(heatStartBlocker(299999, BLOCKER_NONE) == BLOCKER_POWER_UP, "heat waits for the power-up delay");
  unitExpect(heatStartBlocker(300000, BLOCKER_NONE) == BLOCKER_NONE, "heat free at 5 min");
  unitExpect(coolStartBlocker(299999, BLOCKER_NONE) == BLOCKER_POWER_UP, "cool waits for the power-up delay");
  unitExpect(coolStartBlocker(300000, BLOCKER_NONE) == BLOCKER_NONE, "cool free at 5 min");
  unitExpect(heatStartBlocker(600000, BLOCKER_SENSOR_LOCKOUT) == BLOCKER_SENSOR_LOCKOUT, "a relay hold comes first");
  sensorState = SENSOR_STATE_STARTING;
  unitExpect(heatStartBlocker(600000, BLOCKER_NONE) == BLOCKER_SENSOR_STARTING, "heat waits for the first reading");
  unitExpect(coolStartBlocker(600000, BLOCKER_NONE) == BLOCKER_SENSOR_STARTING, "cool waits for the first reading");
  sensorState = SENSOR_STATE_OK;
  coolLastOffMs = 400000;
  unitExpect(heatStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CHANGEOVER, "heat waits out the dead band after cooling");
  unitExpect(startBlockerRemainingMs(BLOCKER_CHANGEOVER, true, 600000) == 100000, "dead band remaining 100 s");
  unitExpect(coolStartBlocker(600000, BLOCKER_NONE) == BLOCKER_COMPRESSOR_OFF_TIME, "cool waits for the compressor off time");
  unitExpect(startBlockerRemainingMs(BLOCKER_COMPRESSOR_OFF_TIME, false, 600000) == 100000, "off time remaining 100 s");
  coolLastOffMs = 0;
  relayState.coolY1 = true;
  unitExpect(heatStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CHANGEOVER, "heat waits while cooling runs");
  unitExpect(startBlockerRemainingMs(BLOCKER_CHANGEOVER, true, 600000) == CHANGEOVER_DEAD_BAND_MS, "full dead band after cooling");
  relayState.coolY1 = false;
  heatLastOffMs = 500000;
  unitExpect(coolStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CHANGEOVER, "cool waits out the dead band after heating");
  heatLastOffMs = 0;
  relayState.heatW1 = true;
  unitExpect(coolStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CHANGEOVER, "cool waits while heating runs");
  relayState.heatW1 = false;
  for (uint8_t start = 0; start < HEAT_STARTS_PER_HOUR_MAX; start++) {
    recordStart(heatStarts, 300000 + start * 1000);
  }
  unitExpect(heatStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CYCLE_LIMIT, "heat waits at 6 starts in the hour");
  for (uint8_t start = 0; start < COOL_STARTS_PER_HOUR_MAX; start++) {
    recordStart(coolStarts, 300000 + start * 1000);
  }
  unitExpect(coolStartBlocker(600000, BLOCKER_NONE) == BLOCKER_CYCLE_LIMIT, "cool waits at 4 starts in the hour");
  unitExpect(startBlockerRemainingMs(BLOCKER_CYCLE_LIMIT, false, 600000) == 3300000, "until the first start is an hour old");
  setUptimeMs(600000);
  noteStartBlocker("cool", coolBlocker, BLOCKER_CYCLE_LIMIT, false, 600000);
  noteStartBlocker("cool", coolBlocker, BLOCKER_CYCLE_LIMIT, false, 601000);
  unitExpect(loggedCount("cool call waiting: starts per hour limit, 3300s left") == 1, "a waiting call is logged once");
}

// ==== starts per hour history ====

static void testStartHistory() {
  unitSection("starts per hour history");
  StartHistory history = {};
  for (uint64_t minute = 0; minute < 8; minute++) {
    recordStart(history, minute * MINUTE_MS);
  }
  recordStart(history, 8 * MINUTE_MS);
  unitExpect(history.count == 8 && history.startMs[0] == 1 * MINUTE_MS && history.startMs[7] == 8 * MINUTE_MS,
             "a full history drops its oldest start");
  pruneStartHistory(history, 61 * MINUTE_MS);
  unitExpect(history.count == 7 && history.startMs[0] == 2 * MINUTE_MS, "starts an hour old are dropped (count %u)", history.count);
  uint64_t remainingMs = cycleLimitRemainingMs(history, 4, 61 * MINUTE_MS);
  unitExpect(remainingMs == 4 * MINUTE_MS, "4 of 7 starts: the 4th newest leaves the hour in 4 min (got %llu ms)",
             (unsigned long long)remainingMs);
  unitExpect(cycleLimitRemainingMs(history, 8, 61 * MINUTE_MS) == 0, "below the limit nothing waits");
}

// ==== final interlock ====

static RelayOutputs outputs(bool heatW1, bool coolY1, bool fanG) {
  RelayOutputs relays;
  relays.heatW1 = heatW1;
  relays.coolY1 = coolY1;
  relays.fanG = fanG;
  return relays;
}

static bool sameOutputs(RelayOutputs first, RelayOutputs second) {
  return (first.heatW1 == second.heatW1) && (first.coolY1 == second.coolY1) && (first.fanG == second.fanG);
}

static void testInterlocks() {
  unitSection("final interlock");
  resetThermoState();
  uint64_t nowMs = 20 * MINUTE_MS;
  setUptimeMs(nowMs);
  RelayOutputs allowed = applyInterlocks(outputs(true, true, true), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, false, true)) && interlockRefusals == 1, "W1 + Y1 together: both held open");
  unitExpect(logged("ERROR interlock: W1 and Y1 requested together, both held open"), "the refusal is logged as an error");
  applyInterlocks(outputs(true, true, true), nowMs);
  unitExpect(interlockRefusals == 1, "the same refusal is counted once");
  allowed = applyInterlocks(outputs(false, false, false), nowMs);
  unitExpect(lastInterlockMessage == nullptr && sameOutputs(allowed, outputs(false, false, false)), "a good request clears it");
  allowed = applyInterlocks(outputs(false, true, false), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, true, true)) && interlockRefusals == 2, "Y1 without G: G closed with it");
  coolLastOffMs = nowMs - 60000;
  allowed = applyInterlocks(outputs(false, true, true), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, false, true)), "Y1 inside the compressor off time: refused");
  coolLastOffMs = 0;
  heatLastOffMs = nowMs - 60000;
  allowed = applyInterlocks(outputs(false, true, true), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, false, true)), "Y1 inside the dead band after heat: refused");
  heatLastOffMs = 0;
  coolLastOffMs = nowMs - 120000;
  allowed = applyInterlocks(outputs(true, false, false), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, false, false)), "W1 inside the dead band after cool: refused");
  coolLastOffMs = 0;
  relayState = outputs(false, true, true);
  allowed = applyInterlocks(outputs(true, false, true), nowMs);
  unitExpect(!allowed.heatW1, "W1 refused in the pass that opens Y1");
  allowed = applyInterlocks(outputs(false, true, true), nowMs);
  unitExpect(sameOutputs(allowed, outputs(false, true, true)), "a running compressor passes");
  relayState = outputs(true, false, false);
  allowed = applyInterlocks(outputs(false, true, true), nowMs);
  unitExpect(!allowed.coolY1, "Y1 refused while W1 is closed");
  interlockRefusals = 0;
}

// ==== relay switching order ====

static bool writesAre(const uint8_t *expectedPins, const int *expectedLevels, uint32_t expectedCount) {
  uint8_t pins[16];
  int levels[16];
  uint32_t count = simRecentRelayWrites(pins, levels, 16);
  if (count != expectedCount) {
    return false;
  }
  for (uint32_t index = 0; index < count; index++) {
    if (pins[index] != expectedPins[index] || levels[index] != expectedLevels[index]) {
      return false;
    }
  }
  return true;
}

static void testDriveOrder() {
  unitSection("relay switching order");
  resetThermoState();
  simResetPins();
  initRelays();
  unitExpect(!simRelayClosed(SIM_PIN_W1) && !simRelayClosed(SIM_PIN_Y1) && !simRelayClosed(SIM_PIN_G), "initRelays opens every contact");
  bool w1Drive12mA = simPinDriveStrength(SIM_PIN_W1) == GPIO_DRIVE_STRENGTH_12MA;
  bool y1Drive12mA = simPinDriveStrength(SIM_PIN_Y1) == GPIO_DRIVE_STRENGTH_12MA;
  bool gDrive12mA = simPinDriveStrength(SIM_PIN_G) == GPIO_DRIVE_STRENGTH_12MA;
  unitExpect(w1Drive12mA && y1Drive12mA && gDrive12mA, "initRelays sets every relay pin to 12 mA drive");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  setUptimeMs(20 * MINUTE_MS);
  simClearRelayWrites();
  driveRelays(outputs(false, true, true), 20 * MINUTE_MS);
  const uint8_t startPins[] = { SIM_PIN_G, SIM_PIN_Y1 };
  const int startLevels[] = { HIGH, HIGH };
  unitExpect(writesAre(startPins, startLevels, 2), "cool start: G closes before Y1");
  unitExpect(logged("cool on (G, Y1 closed)"), "cool start logged");
  setUptimeMs(26 * MINUTE_MS);
  simClearRelayWrites();
  coolChangeReason = "setpoint reached";
  driveRelays(outputs(false, false, false), 26 * MINUTE_MS);
  const uint8_t stopPins[] = { SIM_PIN_Y1, SIM_PIN_G };
  const int stopLevels[] = { LOW, LOW };
  unitExpect(writesAre(stopPins, stopLevels, 2), "cool stop: Y1 opens before G");
  unitExpect(logged("cool off (Y1, G opened) after 360s: setpoint reached"), "cool stop logged with its run time and reason");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  setUptimeMs(32 * MINUTE_MS);
  driveRelays(outputs(true, false, false), 32 * MINUTE_MS);
  setUptimeMs(36 * MINUTE_MS);
  simClearRelayWrites();
  driveRelays(outputs(false, false, true), 36 * MINUTE_MS);
  const uint8_t swapPins[] = { SIM_PIN_W1, SIM_PIN_G };
  const int swapLevels[] = { LOW, HIGH };
  unitExpect(writesAre(swapPins, swapLevels, 2), "contacts open before any closes");
  unitExpect(heatStarts.count == 1 && coolStarts.count == 1, "starts recorded");
  unitExpect(runningState() == RUNNING_FAN_ONLY, "G alone is fan only");
  openAllRelays();
  unitExpect(!simRelayClosed(SIM_PIN_G), "openAllRelays opens every contact");
}

// ==== fan decision ====

static void testFanDecision() {
  unitSection("fan (G) decision");
  resetThermoState();
  uint64_t nowMs = 30 * MINUTE_MS;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  unitExpect(decideFanRelay(nowMs, true, BLOCKER_NONE), "G with every cool call");
  unitExpect(!decideFanRelay(nowMs, true, BLOCKER_SENSOR_LOCKOUT), "G open during the lockout");
  settings.fanMode = (uint8_t)FAN_MODE_ON;
  unitExpect(decideFanRelay(nowMs, false, BLOCKER_NONE), "G closed in fan on");
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  unitExpect(decideFanRelay(nowMs, false, BLOCKER_NONE), "fan on with mode off: fan only");
  settings.fanMode = (uint8_t)FAN_MODE_AUTO;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  coolRunOnAllowed = true;
  coolLastOffMs = nowMs - 30000;
  unitExpect(decideFanRelay(nowMs, false, BLOCKER_NONE), "G runs on 30 s after a cool cycle");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  unitExpect(decideFanRelay(nowMs, false, BLOCKER_NONE), "G runs on after a change to heat");
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  unitExpect(!decideFanRelay(nowMs, false, BLOCKER_NONE), "off ends the run-on");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  coolLastOffMs = nowMs - 60000;
  unitExpect(!decideFanRelay(nowMs, false, BLOCKER_NONE), "run-on over at 60 s");
  relayState.coolY1 = true;
  unitExpect(decideFanRelay(nowMs, false, BLOCKER_NONE), "run-on starts in the pass that opens Y1");
  coolRunOnAllowed = false;
  unitExpect(!decideFanRelay(nowMs, false, BLOCKER_NONE), "no run-on after a safety stop");
  relayState.coolY1 = false;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  unitExpect(!decideFanRelay(nowMs, false, BLOCKER_NONE), "fan auto on heat: G open (the furnace runs its blower)");

  // the Wi-Fi restart waits for hvacCycleRunning() to clear: W1 / Y1 and the cool run-on
  resetThermoState();
  setUptimeMs(nowMs);
  relayState = outputs(false, false, true);
  coolRunOnAllowed = true;
  coolLastOffMs = nowMs - 30000;
  unitExpect(hvacCycleRunning(), "a cycle is still running during the blower run-on");
  coolLastOffMs = nowMs - 60000;
  unitExpect(!hvacCycleRunning(), "the cycle is over once the run-on ends");
  coolRunOnAllowed = false;
  coolLastOffMs = nowMs - 30000;
  unitExpect(!hvacCycleRunning(), "fan on alone (no run-on) is not a cycle");
  relayState = outputs(true, false, false);
  unitExpect(hvacCycleRunning(), "W1 closed is a running cycle");
  relayState = outputs(false, true, true);
  unitExpect(hvacCycleRunning(), "Y1 closed is a running cycle");
}

// ==== safety stops ====

static void testSafetyStops() {
  unitSection("safety stops");
  resetThermoState();
  uint64_t nowMs = 30 * MINUTE_MS;
  setUptimeMs(nowMs);
  relayState = outputs(false, true, true);
  coolOnSinceMs = nowMs - 60000;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  sensorState = SENSOR_STATE_FAILED;
  unitExpect(!keepCooling(nowMs, BLOCKER_NONE) && strcmp(coolChangeReason, "sensor failed") == 0 && !coolRunOnAllowed,
             "a failed sensor stops Y1 at once, inside its minimum, without run-on");
  unitExpect(!startCooling(nowMs, BLOCKER_NONE), "no cool start with a failed sensor");
  sensorState = SENSOR_STATE_OK;
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  unitExpect(!keepCooling(nowMs, BLOCKER_NONE) && strcmp(coolChangeReason, "mode off") == 0, "off stops Y1 at once");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  unitExpect(keepCooling(nowMs, BLOCKER_NONE), "a change to heat waits for the compressor minimum");
  unitExpect(!keepCooling(nowMs, BLOCKER_SENSOR_LOCKOUT), "the lockout stops Y1");
  relayState = outputs(true, false, false);
  heatOnSinceMs = nowMs - 60000;
  unitExpect(!keepHeating(nowMs, BLOCKER_SENSOR_LOCKOUT) && strcmp(heatChangeReason, "sensor lockout") == 0, "the lockout stops W1");
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  unitExpect(!keepHeating(nowMs, BLOCKER_NONE) && strcmp(heatChangeReason, "mode off") == 0, "off stops W1 at once");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  unitExpect(keepHeating(nowMs, BLOCKER_NONE), "a change to cool waits for the furnace minimum");
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  settingsSavePending = false;
  switchCoolOffForSensor("DHT22 failed");
  unitExpect(settings.hvacMode == HVAC_MODE_OFF && settingsSavePending, "a failed sensor switches cool mode off");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  switchCoolOffForSensor("DHT22 failed");
  unitExpect(settings.hvacMode == HVAC_MODE_HEAT, "heat mode is left alone");
}

// ==== stale sensor: heat and cool paused ====

static void testStaleSensor() {
  unitSection("stale sensor: nothing starts, running cycles stop after their minimum");
  uint64_t nowMs = 30 * MINUTE_MS;
  resetThermoState();
  setUptimeMs(nowMs);
  sensorState = SENSOR_STATE_STALE;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorTemperatureTenthsF = 650; // the last reading still asks for heat
  unitExpect(heatStartBlocker(nowMs, BLOCKER_NONE) == BLOCKER_SENSOR_STALE, "heat start blocked while stale");
  unitExpect(!startHeating(nowMs, BLOCKER_NONE), "no furnace start on an old reading");
  unitExpect(logged("heat call waiting: sensor stale"), "the waiting call is logged");
  relayState = outputs(true, false, false);
  heatOnSinceMs = nowMs - 60000;
  unitExpect(keepHeating(nowMs, BLOCKER_NONE), "a running furnace finishes its minimum on time");
  unitExpect(logged("heat sensor stale, held for furnace minimum on time, 120s left"), "the hold is logged");
  heatOnSinceMs = nowMs - 180000;
  unitExpect(!keepHeating(nowMs, BLOCKER_NONE) && strcmp(heatChangeReason, "sensor stale") == 0, "then the furnace stops");

  resetThermoState();
  setUptimeMs(nowMs);
  sensorState = SENSOR_STATE_STALE;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  sensorTemperatureTenthsF = 800; // the last reading still asks for cooling
  unitExpect(coolStartBlocker(nowMs, BLOCKER_NONE) == BLOCKER_SENSOR_STALE, "cool start blocked while stale");
  unitExpect(!startCooling(nowMs, BLOCKER_NONE), "no compressor start on an old reading");
  relayState = outputs(false, true, true);
  coolOnSinceMs = nowMs - 120000;
  unitExpect(keepCooling(nowMs, BLOCKER_NONE), "a running compressor finishes its minimum on time");
  coolOnSinceMs = nowMs - 300000;
  bool stopped = !keepCooling(nowMs, BLOCKER_NONE);
  unitExpect(stopped && strcmp(coolChangeReason, "sensor stale") == 0 && coolRunOnAllowed, "then it stops, with the blower run-on");

  resetThermoState();
  setUptimeMs(nowMs);
  sensorState = SENSOR_STATE_FAILED;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  sensorLastValidMs = nowMs - 600000;
  unitExpect(startHeating(nowMs, BLOCKER_NONE), "once failed, the 25 %% fallback starts the furnace");
}

// ==== status text through the API routes ====

static void testStatusText() {
  unitSection("status text (?10,0,0~ and ?11,0,0~)");
  resetThermoState();
  startHttpServer(runCommand);
  uint64_t nowMs = 30 * MINUTE_MS;
  setUptimeMs(nowMs);
  sensorState = SENSOR_STATE_FAILED;
  sensorLastValidMs = nowMs - 700000;
  sensorLastResult = DHT_RESULT_NO_RESPONSE;
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  std::string status = simRunCommand(10, 0);
  unitExpect(status.find("heat running 25% fallback until the sensor reads again") != std::string::npos, "failed in heat: fallback error");
  unitExpect(status.find("DHT22 failed, no reading for 700s (no response)") != std::string::npos, "failed: time without a reading");
  unitExpect(status.find("not used: sensor failed, heat runs 25%") != std::string::npos, "failed in heat: setpoint not used");
  unitExpect(status.find("temperature:         NA") != std::string::npos, "failed: temperature NA");
  std::string statusLine = simRunCommand(11, 0);
  unitExpect(statusLine == "1~0~70~NA~NA~4~0~0~0~0", "?11,0,0~ while failed (got \"%s\")", statusLine.c_str());
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  status = simRunCommand(10, 0);
  unitExpect(status.find("cool not allowed until the sensor reads again") != std::string::npos, "failed in off: cool not allowed");
  sensorState = SENSOR_STATE_LOCKOUT;
  sensorHasReading = false;
  status = simRunCommand(10, 0);
  unitExpect(status.find("HVAC locked out, every relay open") != std::string::npos, "lockout error");
  unitExpect(status.find("no reading since boot") != std::string::npos, "lockout: no reading since boot");
  sensorState = SENSOR_STATE_STALE;
  sensorHasReading = true;
  sensorLastValidMs = nowMs - 90000;
  status = simRunCommand(10, 0);
  unitExpect(status.find("DHT22 no reading for 90s (no response): heat/cool paused, fails in 510s") != std::string::npos,
             "stale: time to failure");
  unitExpect(status.find("70.0 F (last reading)") != std::string::npos, "stale: last reading shown");
  sensorState = SENSOR_STATE_OK;
  sensorLastResult = DHT_RESULT_OK;
  settings.hvacMode = (uint8_t)HVAC_MODE_COOL;
  status = simRunCommand(10, 0);
  unitExpect(status.find("70 F (cool on at 70.5 F, off at 69.5 F)") != std::string::npos, "cool switching temperatures shown");
  settings.hvacMode = (uint8_t)HVAC_MODE_HEAT;
  status = simRunCommand(10, 0);
  unitExpect(status.find("70 F (heat on at 69.5 F, off at 70.5 F)") != std::string::npos, "heat switching temperatures shown");
  interlockRefusals = 2;
  status = simRunCommand(10, 0);
  unitExpect(status.find("2 relay requests overridden by the interlocks") != std::string::npos, "interlock overrides reported");
  interlockRefusals = 0;
  settings.coolShutOff = 1;
  status = simRunCommand(10, 0);
  unitExpect(status.find("cool was switched off by a sensor failure") != std::string::npos, "the cool shut-off is reported");
  simRunCommand(4, 0);
  status = simRunCommand(10, 0);
  unitExpect(settings.coolShutOff == 0 && status.find("cool was switched off") == std::string::npos, "setting the mode clears it");
}

// ==== names ====

static void testNames() {
  unitSection("names");
  const char *sensorNames[] = { "ok", "starting", "startup lockout", "stale", "failed" };
  for (int16_t state = 0; state <= SENSOR_STATE_FAILED; state++) {
    unitExpect(strcmp(sensorStateName(state), sensorNames[state]) == 0, "sensor state %d is \"%s\"", state, sensorNames[state]);
  }
  unitExpect(strcmp(sensorStateName(99), "unknown") == 0, "an unknown sensor state is \"unknown\"");
  const char *runningNames[] = { "idle", "heating", "cooling", "fan only" };
  for (int16_t state = 0; state <= RUNNING_FAN_ONLY; state++) {
    unitExpect(strcmp(runningStateName(state), runningNames[state]) == 0, "running state %d is \"%s\"", state, runningNames[state]);
  }
  unitExpect(strcmp(dhtResultName(DHT_RESULT_BAD_CHECKSUM), "bad checksum") == 0, "DHT result names");
  unitExpect(strcmp(startBlockerName(BLOCKER_CYCLE_LIMIT), "starts per hour limit") == 0, "blocker names");
  unitExpect(strcmp(startBlockerName(BLOCKER_SENSOR_STALE), "sensor stale") == 0, "the stale blocker's name");
}

// Runs every unit test (in a forked child, on a world prepared by the runner); returns the failures.
int runUnitTests() {
  recursive_mutex_init(&stateMutex);
  testPinsAndConstants();
  testTimeAndTextHelpers();
  testDhtDecode();
  testDhtLine();
  testBelievableReadings();
  testReadSchedule();
  testSensorStates();
  testDisplayReadings();
  testBacklightLog();
  testRunLimits();
  testHeatFallback();
  testThresholds();
  testBlockers();
  testStartHistory();
  testInterlocks();
  testDriveOrder();
  testFanDecision();
  testSafetyStops();
  testStaleSensor();
  testStatusText();
  testNames();
  world->scenarioChecks = unitChecks;
  world->scenarioFailures = unitFailures;
  tracef("UNIT", "%u unit checks, %u failed", unitChecks, unitFailures);
  return (int)unitFailures;
}
