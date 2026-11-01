// =====================================================================================
// thermo.cpp
// SmartThermo: the DHT22 sensor, the thermostat decisions, the relays and their interlocks,
// and the thermostat's sections of the status reply. See thermo.h.
// =====================================================================================
#include <hardware/gpio.h>
#include <hardware/timer.h>

#include "thermo.h"
#include "http.h"

// ---- temperature control ----
// differential (swing) centred on the setpoint: at 70 F heat runs from 69.5 F up to 70.5 F,
// and cooling from 70.5 F down to 69.5 F
const int32_t TEMPERATURE_SWING_TENTHS_F = 10;
const int32_t HALF_SWING_TENTHS_F = TEMPERATURE_SWING_TENTHS_F / 2;
// heat with a failed sensor: W1 runs HEAT_FALLBACK_ON_MS at the start of every HEAT_FALLBACK_PERIOD_MS (25%)
const uint64_t HEAT_FALLBACK_PERIOD_MS = 1200000; // 20 min
const uint64_t HEAT_FALLBACK_ON_MS = 300000;      // 5 min
const uint8_t HEAT_FALLBACK_PERCENT = 25;

// ---- running state names ----
const char *const RUNNING_NAMES[] = { "idle", "heating", "cooling", "fan only" };
const uint8_t RUNNING_NAME_COUNT = sizeof(RUNNING_NAMES) / sizeof(RUNNING_NAMES[0]);
static_assert(RUNNING_NAME_COUNT == RUNNING_FAN_ONLY + 1, "one name per running state");

// ---- HVAC lockout and dead time (HVAC Thermostat Control Reference) ----
const uint64_t COMPRESSOR_MIN_OFF_MS = 300000;   // 5 min after every Y1 stop before Y1 may close again
const uint64_t COMPRESSOR_MIN_ON_MS = 300000;    // 5 min (3-5 recommended), unless stopped by off or a sensor fault
const uint64_t FURNACE_MIN_ON_MS = 180000;       // 3 min, unless stopped by off or the sensor lockout
const uint64_t CHANGEOVER_DEAD_BAND_MS = 300000; // 5 min with W1 and Y1 both open between heat and cool
const uint64_t COOL_FAN_RUN_ON_MS = 60000;       // G held 60 s after a cool cycle (0-90 s recommended)
const uint64_t CYCLE_WINDOW_MS = 3600000;        // starts are counted over the last hour
const uint8_t COOL_STARTS_PER_HOUR_MAX = 4;      // 3-4 recommended
const uint8_t HEAT_STARTS_PER_HOUR_MAX = 6;      // 5-6 recommended
const uint8_t START_HISTORY_CAPACITY = 8;        // more than either limit
static_assert(COOL_STARTS_PER_HOUR_MAX <= START_HISTORY_CAPACITY, "the start history must hold the cool limit");
static_assert(HEAT_STARTS_PER_HOUR_MAX <= START_HISTORY_CAPACITY, "the start history must hold the heat limit");
static_assert(COOL_FAN_RUN_ON_MS < CHANGEOVER_DEAD_BAND_MS, "the blower run-on must end inside the dead band");
static_assert(HEAT_FALLBACK_ON_MS >= FURNACE_MIN_ON_MS, "a fallback heat run must cover the furnace minimum on time");
static_assert(HEAT_FALLBACK_ON_MS * 100 / HEAT_FALLBACK_PERIOD_MS == HEAT_FALLBACK_PERCENT, "the fallback duty must match its percentage");

// ---- run limits: how long W1 and Y1 may run, see updateRunLimits ----
const uint64_t HEAT_RESPONSE_WINDOW_MS = 3600000; // 1 h for W1 to warm the room by RESPONSE_MIN_CHANGE_TENTHS_F
const uint64_t COOL_RESPONSE_WINDOW_MS = 7200000; // 2 h for Y1 to cool it by the same
const int32_t RESPONSE_MIN_CHANGE_TENTHS_F = 10;  // 1.0 F, more than the sensor's noise
const uint64_t HEAT_MAX_RUN_MS = 10800000;        // 3 h, the longest W1 stays closed, whatever the temperature does
const uint64_t COOL_MAX_RUN_MS = 21600000;        // 6 h, the same for Y1
const uint64_t RUN_LIMIT_REST_MS = 1800000;       // 30 min off after a stop at a limit
const uint8_t FROZEN_READING_STOPS = 3;           // no-response stops in a row, the reading never moving, that end in the last step
static_assert(HEAT_RESPONSE_WINDOW_MS >= FURNACE_MIN_ON_MS, "a run limit must never cut the furnace minimum on time");
static_assert(COOL_RESPONSE_WINDOW_MS >= COMPRESSOR_MIN_ON_MS, "a run limit must never cut the compressor minimum on time");
static_assert(RUN_LIMIT_REST_MS >= COMPRESSOR_MIN_OFF_MS, "the rest must cover the compressor off time");

// the start times of one stage within the last hour, oldest first
struct StartHistory {
  uint64_t startMs[START_HISTORY_CAPACITY];
  uint8_t count;
};
StartHistory heatStarts = {};
StartHistory coolStarts = {};

// why a wanted heat or cool start is waiting; BLOCKER_SENSOR_LOCKOUT also holds every relay open
enum StartBlocker {
  BLOCKER_NONE,
  BLOCKER_SENSOR_LOCKOUT,
  BLOCKER_POWER_UP,
  BLOCKER_SENSOR_STARTING,
  BLOCKER_SENSOR_STALE,
  BLOCKER_CHANGEOVER,
  BLOCKER_COMPRESSOR_OFF_TIME,
  BLOCKER_CYCLE_LIMIT,
  BLOCKER_RUN_LIMIT_REST,
  BLOCKER_COUNT
};
// the name each blocker is logged under, in StartBlocker order
const char *const START_BLOCKER_NAMES[] = {
  "none", "sensor lockout", "power-up delay", "sensor starting", "sensor stale", "heat/cool dead band",
  "compressor off time", "starts per hour limit", "run limit rest"
};
static_assert(sizeof(START_BLOCKER_NAMES) / sizeof(START_BLOCKER_NAMES[0]) == BLOCKER_COUNT, "one name per StartBlocker");

// ---- relays ----
RelayOutputs relayState = { false, false, false }; // what the pins drive now
uint64_t heatOnSinceMs = 0;   // when W1 last closed
uint64_t heatLastOffMs = 0;   // when W1 last opened (boot counts)
uint64_t coolOnSinceMs = 0;   // when Y1 last closed
uint64_t coolLastOffMs = 0;   // when Y1 last opened (boot counts)
bool coolRunOnAllowed = false;      // true when the last cool cycle ended normally, so G may run on
bool heatMinOnHoldLogged = false;   // the minimum on time hold is logged once per cycle
bool coolMinOnHoldLogged = false;
const char *heatChangeReason = "";  // why W1 is about to open, for the log
const char *coolChangeReason = "";  // why Y1 is about to open, for the log
const char *fanChangeReason = "";   // why G is closed or open, for the log
StartBlocker heatBlocker = BLOCKER_NONE; // why a wanted heat start is waiting
StartBlocker coolBlocker = BLOCKER_NONE;
const char *lastInterlockMessage = nullptr; // last interlock refusal logged, cleared once requests pass
uint32_t interlockRefusals = 0;           // relay requests the interlocks overrode
bool responseTracking = false;            // true while a running stage is being watched on fresh readings
uint64_t responseCheckpointMs = 0;        // when the room last moved the right way by RESPONSE_MIN_CHANGE_TENTHS_F
int32_t responseCheckpointTenthsF = 0;    // the temperature at that moment
bool noResponseAlarm = false;             // true from a no-response stop until the room responds to a later run
bool runLimitStopDue = false;             // the running stage opens this pass
const char *runLimitStopReason = "";      // why, for the log
const char *runLimitStageName = "";       // the stage stopped last, for the status
bool runLimitResting = false;             // true during the rest: neither stage may start
uint64_t runLimitRestStartMs = 0;         // when the rest began
uint8_t frozenStopCount = 0;              // no-response stops in a row with the reading never moving
int32_t frozenReferenceTenthsF = 0;       // the reading at the first of them
bool frozenReadingActive = false;         // true while heat runs the 25% fallback because the reading is frozen
uint64_t frozenFallbackStartMs = 0;       // when that fallback's first period starts

// ---- DHT22 temperature and humidity sensor ----
const uint64_t DHT_POWER_UP_MS = 2000;       // the sensor needs 1-2 s after power before its first reading
const uint64_t DHT_READ_INTERVAL_MS = 5000;  // the DHT22 allows one reading every 2 s
const uint64_t DHT_STARTUP_TIMEOUT_MS = 60000; // no reading this long after boot = startup lockout
const uint64_t DHT_STALE_MS = 60000;         // no valid reading for this long = stale (heat and cool paused)
const uint64_t DHT_FAILED_MS = 600000;       // no valid reading for 10 min = failed (heat 25%, cool off)
const uint32_t DHT_START_LOW_US = 1100;      // host start signal, at least 1 ms low
const uint32_t DHT_RELEASE_WAIT_US = 55;     // the sensor answers 20-40 us after release with 80 us low
const uint32_t DHT_PULSE_TIMEOUT_US = 120;   // longest pulse expected: 80 us response, 50 us + 26-70 us bits
const uint32_t DHT_PULSE_TIMED_OUT = 0xFFFFFFFF;
const uint8_t DHT_DATA_BITS = 40;
const uint8_t DHT_DATA_BYTES = 5;
const uint8_t DHT_CHECKSUM_INDEX = 4;        // byte 4 is the sum of bytes 0-3
const uint8_t BITS_PER_BYTE = 8;
const uint8_t BYTE_MASK = 0xFF;
const uint8_t DHT_NEGATIVE_FLAG = 0x80;      // top bit of the temperature's high byte
const uint8_t DHT_TEMPERATURE_HIGH_MASK = 0x7F;
const int32_t DHT_TEMPERATURE_MIN_TENTHS_C = -400; // DHT22 range -40 to 80 C
const int32_t DHT_TEMPERATURE_MAX_TENTHS_C = 800;
const int32_t DHT_PLAUSIBLE_MIN_TENTHS_F = 200;    // an indoor reading outside 20-120 F is a bad frame
const int32_t DHT_PLAUSIBLE_MAX_TENTHS_F = 1200;
const uint16_t DHT_PLAUSIBLE_MIN_HUMIDITY_TENTHS = 10; // under 1.0 %; an all-zero frame passes its checksum and lands here
const int32_t DHT_JUMP_LIMIT_TENTHS_F = 50;        // a reading further than 5 F from the last must be repeated
const uint16_t DHT_HUMIDITY_MAX_TENTHS = 1000;     // 100.0 %
const int32_t FAHRENHEIT_SCALE_NUMERATOR = 9;      // F = C * 9 / 5 + 32
const int32_t FAHRENHEIT_SCALE_DENOMINATOR = 5;
const int32_t FAHRENHEIT_OFFSET_TENTHS = 320;

enum DhtResult {
  DHT_RESULT_OK,
  DHT_RESULT_NO_RESPONSE,
  DHT_RESULT_DATA_TIMEOUT,
  DHT_RESULT_BAD_CHECKSUM,
  DHT_RESULT_OUT_OF_RANGE,
  DHT_RESULT_IMPLAUSIBLE,
  DHT_RESULT_UNCONFIRMED_JUMP,
  DHT_RESULT_COUNT
};
// the name each result is logged under, in DhtResult order
const char *const DHT_RESULT_NAMES[] = { "ok", "no response", "data timeout", "bad checksum", "reading out of range",
                                         "implausible reading", "unconfirmed jump" };
static_assert(sizeof(DHT_RESULT_NAMES) / sizeof(DHT_RESULT_NAMES[0]) == DHT_RESULT_COUNT, "one name per DhtResult");
const char *const SENSOR_STATE_NAMES[] = { "ok", "starting", "startup lockout", "stale", "failed" };
const uint8_t SENSOR_STATE_NAME_COUNT = sizeof(SENSOR_STATE_NAMES) / sizeof(SENSOR_STATE_NAMES[0]);
static_assert(SENSOR_STATE_NAME_COUNT == SENSOR_STATE_FAILED + 1, "one name per sensor state");
static_assert(DHT_STALE_MS < DHT_FAILED_MS, "a sensor goes stale before it fails");

int16_t sensorState = SENSOR_STATE_STARTING;
bool sensorHasReading = false;          // a valid reading has arrived since boot
int32_t sensorTemperatureTenthsF = 0;   // last valid reading
uint16_t sensorHumidityTenths = 0;      // last valid reading, tenths of a percent
uint64_t sensorLastValidMs = 0;         // when the last valid reading arrived
uint64_t sensorNextReadMs = 0;          // when the next reading is due
uint32_t sensorReadsValid = 0;
uint32_t sensorReadsFailed = 0;
uint32_t sensorConsecutiveFailures = 0;
DhtResult sensorLastResult = DHT_RESULT_OK;
bool sensorJumpPending = false;         // a reading far from the last is waiting to be repeated
int32_t sensorJumpTenthsF = 0;          // that reading

// ==== names ====

// Returns the readable name of a sensor state, e.g. 2 -> "startup lockout".
const char *sensorStateName(int16_t state) {
  uint16_t stateIndex = (uint16_t)state;
  return lookupName(SENSOR_STATE_NAMES, SENSOR_STATE_NAME_COUNT, stateIndex, "unknown");
}

// Returns the readable name of a running state, e.g. 2 -> "cooling".
const char *runningStateName(int16_t state) {
  uint16_t stateIndex = (uint16_t)state;
  return lookupName(RUNNING_NAMES, RUNNING_NAME_COUNT, stateIndex, "unknown");
}

// Returns the readable name of a start blocker, e.g. BLOCKER_COMPRESSOR_OFF_TIME -> "compressor off time".
const char *startBlockerName(StartBlocker blocker) {
  return lookupName(START_BLOCKER_NAMES, BLOCKER_COUNT, blocker, "unknown");
}

// Returns the readable name of a DHT22 read result, e.g. DHT_RESULT_BAD_CHECKSUM -> "bad checksum".
const char *dhtResultName(DhtResult result) {
  return lookupName(DHT_RESULT_NAMES, DHT_RESULT_COUNT, result, "unknown");
}

// ==== DHT22 sensor ====

// Waits while the DHT22 data line stays at `level` and returns how long it stayed, in
// microseconds, or DHT_PULSE_TIMED_OUT once it has stayed longer than timeoutUs.
uint32_t measureDhtPulseUs(int level, uint32_t timeoutUs) {
  uint32_t startUs = time_us_32();
  while (true) {
    int lineLevel = digitalRead(DHT22_DATA_PIN);
    uint32_t elapsedUs = time_us_32() - startUs;
    bool levelEnded = (lineLevel != level);
    if (levelEnded) {
      return elapsedUs;
    }
    bool timedOut = (elapsedUs > timeoutUs);
    if (timedOut) {
      return DHT_PULSE_TIMED_OUT;
    }
  }
}

// Reads the DHT22's 5 raw bytes (humidity high/low, temperature high/low, checksum) into
// dataBytes. The bits are told apart by their pulse lengths (26-28 us high = 0, 70 us = 1, after
// a 50 us low each), so interrupts are held off for the ~5 ms of the reply; the pulses are
// decoded afterwards. Returns DHT_RESULT_OK, DHT_RESULT_NO_RESPONSE or DHT_RESULT_DATA_TIMEOUT.
DhtResult readDht22Bytes(uint8_t *dataBytes) {
  uint32_t lowPulseUs[DHT_DATA_BITS];
  uint32_t highPulseUs[DHT_DATA_BITS];
  // start signal: hold the line low for over 1 ms, then release it to the pull-up
  pinMode(DHT22_DATA_PIN, OUTPUT);
  digitalWrite(DHT22_DATA_PIN, LOW);
  delayMicroseconds(DHT_START_LOW_US);
  noInterrupts();
  pinMode(DHT22_DATA_PIN, INPUT_PULLUP);
  // a busy-wait: delayMicroseconds() would sleep until a timer interrupt taken on core 0
  busy_wait_us_32(DHT_RELEASE_WAIT_US);
  // the sensor answers with 80 us low then 80 us high; a missing sensor leaves the line high
  uint32_t responseLowUs = measureDhtPulseUs(LOW, DHT_PULSE_TIMEOUT_US);
  uint32_t responseHighUs = measureDhtPulseUs(HIGH, DHT_PULSE_TIMEOUT_US);
  bool responseLowOk = (responseLowUs != DHT_PULSE_TIMED_OUT);
  bool responseHighOk = (responseHighUs != DHT_PULSE_TIMED_OUT);
  bool responded = responseLowOk && responseHighOk;
  bool bitsComplete = false;
  if (responded) {
    bitsComplete = true;
    for (uint8_t bitIndex = 0; bitIndex < DHT_DATA_BITS; bitIndex++) {
      lowPulseUs[bitIndex] = measureDhtPulseUs(LOW, DHT_PULSE_TIMEOUT_US);
      highPulseUs[bitIndex] = measureDhtPulseUs(HIGH, DHT_PULSE_TIMEOUT_US);
      bool lowTimedOut = (lowPulseUs[bitIndex] == DHT_PULSE_TIMED_OUT);
      bool highTimedOut = (highPulseUs[bitIndex] == DHT_PULSE_TIMED_OUT);
      if (lowTimedOut || highTimedOut) {
        bitsComplete = false;
        break;
      }
    }
  }
  interrupts();
  if (!responded) {
    return DHT_RESULT_NO_RESPONSE;
  }
  if (!bitsComplete) {
    return DHT_RESULT_DATA_TIMEOUT;
  }
  memset(dataBytes, 0, DHT_DATA_BYTES);
  for (uint8_t bitIndex = 0; bitIndex < DHT_DATA_BITS; bitIndex++) {
    // a bit is 1 when its high pulse outlasts the 50 us low before it
    bool bitIsOne = (highPulseUs[bitIndex] > lowPulseUs[bitIndex]);
    uint8_t byteIndex = bitIndex / BITS_PER_BYTE;
    uint8_t shiftedByte = (uint8_t)(dataBytes[byteIndex] << 1);
    if (bitIsOne) {
      shiftedByte = shiftedByte | 1;
    }
    dataBytes[byteIndex] = shiftedByte;
  }
  return DHT_RESULT_OK;
}

// Checks and converts the DHT22's raw bytes: humidity in tenths of a percent and temperature in
// tenths of a degree C, sign in the top bit. Example: 02 8C 01 5F EE -> 65.2 %, 35.1 C (951 tenths F, 95.1 F).
DhtResult decodeDht22Bytes(const uint8_t *dataBytes, int32_t &temperatureTenthsF, uint16_t &humidityTenths) {
  uint16_t byteSum = sumBytes(dataBytes, DHT_CHECKSUM_INDEX);
  uint8_t expectedChecksum = (uint8_t)(byteSum & BYTE_MASK);
  bool checksumMatches = (expectedChecksum == dataBytes[DHT_CHECKSUM_INDEX]);
  if (!checksumMatches) {
    return DHT_RESULT_BAD_CHECKSUM;
  }
  uint16_t humidityHighByte = dataBytes[0];
  uint16_t humidityLowByte = dataBytes[1];
  uint16_t shiftedHumidityHigh = humidityHighByte << BITS_PER_BYTE;
  uint16_t humidity = shiftedHumidityHigh | humidityLowByte;
  uint16_t temperatureHighByte = dataBytes[2] & DHT_TEMPERATURE_HIGH_MASK;
  uint16_t temperatureLowByte = dataBytes[3];
  uint16_t shiftedTemperatureHigh = temperatureHighByte << BITS_PER_BYTE;
  uint16_t temperatureMagnitude = shiftedTemperatureHigh | temperatureLowByte;
  int32_t temperatureTenthsC = temperatureMagnitude;
  bool belowZero = ((dataBytes[2] & DHT_NEGATIVE_FLAG) != 0);
  if (belowZero) {
    temperatureTenthsC = -temperatureTenthsC;
  }
  bool humidityInRange = (humidity <= DHT_HUMIDITY_MAX_TENTHS);
  bool temperatureAboveMin = (temperatureTenthsC >= DHT_TEMPERATURE_MIN_TENTHS_C);
  bool temperatureBelowMax = (temperatureTenthsC <= DHT_TEMPERATURE_MAX_TENTHS_C);
  bool readingInRange = humidityInRange && temperatureAboveMin && temperatureBelowMax;
  if (!readingInRange) {
    return DHT_RESULT_OUT_OF_RANGE;
  }
  int32_t scaledByNine = temperatureTenthsC * FAHRENHEIT_SCALE_NUMERATOR;
  int32_t dividedByFive = scaledByNine / FAHRENHEIT_SCALE_DENOMINATOR;
  temperatureTenthsF = dividedByFive + FAHRENHEIT_OFFSET_TENTHS;
  humidityTenths = humidity;
  return DHT_RESULT_OK;
}

// Returns true when two temperatures are within DHT_JUMP_LIMIT_TENTHS_F of each other.
bool temperaturesAgree(int32_t firstTenthsF, int32_t secondTenthsF) {
  int32_t difference = firstTenthsF - secondTenthsF;
  bool closeAbove = (difference <= DHT_JUMP_LIMIT_TENTHS_F);
  bool closeBelow = (difference >= -DHT_JUMP_LIMIT_TENTHS_F);
  bool agree = closeAbove && closeBelow;
  return agree;
}

// Checks a decoded reading whose checksum passed, since an 8-bit sum can't catch everything: it must
// be possible indoors, and a reading more than 5 F from the last accepted one only counts once the
// next reading agrees with it. So one bad frame can't start or stop the equipment, while a real
// change is accepted a sample later. The first reading since boot has nothing to compare with.
// Example: 70.0 F accepted, then 50.3 F (unconfirmed jump), then 70.1 F (accepted, 50.3 F forgotten).
DhtResult checkReadingBelievable(int32_t temperatureTenthsF, uint16_t humidityTenths) {
  bool temperaturePlausible = (temperatureTenthsF >= DHT_PLAUSIBLE_MIN_TENTHS_F) && (temperatureTenthsF <= DHT_PLAUSIBLE_MAX_TENTHS_F);
  bool humidityPlausible = (humidityTenths >= DHT_PLAUSIBLE_MIN_HUMIDITY_TENTHS);
  if (!temperaturePlausible || !humidityPlausible) {
    return DHT_RESULT_IMPLAUSIBLE;
  }
  if (!sensorHasReading) {
    return DHT_RESULT_OK;
  }
  bool agreesWithLast = temperaturesAgree(temperatureTenthsF, sensorTemperatureTenthsF);
  bool repeatsJump = sensorJumpPending && temperaturesAgree(temperatureTenthsF, sensorJumpTenthsF);
  if (agreesWithLast || repeatsJump) {
    sensorJumpPending = false;
    return DHT_RESULT_OK;
  }
  sensorJumpPending = true;
  sensorJumpTenthsF = temperatureTenthsF;
  return DHT_RESULT_UNCONFIRMED_JUMP;
}

// Takes a DHT22 reading every DHT_READ_INTERVAL_MS. A valid, believable reading is kept for the
// thermostat; a failure is counted and logged when it follows a good reading, so a sensor that
// stays unplugged doesn't flood the log (its state change is logged by updateSensorState).
void readSensorWhenDue() {
  uint64_t nowMs = uptimeMs();
  bool readIsDue = (nowMs >= sensorNextReadMs);
  if (!readIsDue) {
    return;
  }
  sensorNextReadMs = nowMs + DHT_READ_INTERVAL_MS;
  uint8_t dataBytes[DHT_DATA_BYTES];
  int32_t temperatureTenthsF = 0;
  uint16_t humidityTenths = 0;
  DhtResult result = readDht22Bytes(dataBytes);
  if (result == DHT_RESULT_OK) {
    result = decodeDht22Bytes(dataBytes, temperatureTenthsF, humidityTenths);
  }
  if (result == DHT_RESULT_OK) {
    result = checkReadingBelievable(temperatureTenthsF, humidityTenths);
  }
  bool previousReadOk = (sensorLastResult == DHT_RESULT_OK);
  sensorLastResult = result;
  if (result == DHT_RESULT_OK) {
    sensorTemperatureTenthsF = temperatureTenthsF;
    sensorHumidityTenths = humidityTenths;
    sensorLastValidMs = nowMs;
    sensorHasReading = true;
    sensorReadsValid = sensorReadsValid + 1;
    sensorConsecutiveFailures = 0;
    return;
  }
  sensorReadsFailed = sensorReadsFailed + 1;
  sensorConsecutiveFailures = sensorConsecutiveFailures + 1;
  if (previousReadOk) {
    const char *resultName = dhtResultName(result);
    logMessage("DHT22 read failed: %s", resultName);
  }
}

// Returns the sensor's state from the age of its last valid reading: ok, stale after DHT_STALE_MS,
// failed after DHT_FAILED_MS. With no reading since boot: starting, then startup lockout once
// DHT_STARTUP_TIMEOUT_MS has passed. The lockout ends in failed after DHT_FAILED_MS in heat mode, so
// the 25% fallback also protects the house when a dead sensor is followed by a restart; in off and
// cool it lasts until the sensor reads.
int16_t evaluateSensorState(uint64_t nowMs) {
  if (sensorHasReading) {
    bool readingFailed = intervalElapsed(sensorLastValidMs, DHT_FAILED_MS, nowMs);
    if (readingFailed) {
      return SENSOR_STATE_FAILED;
    }
    bool readingStale = intervalElapsed(sensorLastValidMs, DHT_STALE_MS, nowMs);
    if (readingStale) {
      return SENSOR_STATE_STALE;
    }
    return SENSOR_STATE_OK;
  }
  bool startupOver = intervalElapsed(bootMs, DHT_STARTUP_TIMEOUT_MS, nowMs);
  if (!startupOver) {
    return SENSOR_STATE_STARTING;
  }
  bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
  bool fallbackDue = intervalElapsed(sensorLastValidMs, DHT_FAILED_MS, nowMs);
  if (heatSelected && fallbackDue) {
    return SENSOR_STATE_FAILED;
  }
  return SENSOR_STATE_LOCKOUT;
}

// Returns true while the thermostat can follow the temperature: the reading is fresh or stale.
bool temperatureUsable() {
  bool sensorOk = (sensorState == SENSOR_STATE_OK);
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  bool usable = sensorOk || sensorStale;
  return usable;
}

// Returns the temperature the thermostat controls on, in tenths of a degree F: the last valid
// reading. Only meaningful while temperatureUsable().
int32_t controlTemperatureTenthsF() {
  return sensorTemperatureTenthsF;
}

// Formats the control temperature for logs: "71.3 F", "71.3 F (last reading)" while stale, or
// "no reading" while the sensor is starting, locked out or failed.
void formatControlTemperature(char *output, size_t outputSize) {
  bool usable = temperatureUsable();
  if (!usable) {
    snprintf(output, outputSize, "no reading");
    return;
  }
  char temperatureText[TEMPERATURE_TEXT_SIZE];
  formatTenths(temperatureText, sizeof(temperatureText), sensorTemperatureTenthsF);
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  if (sensorStale) {
    snprintf(output, outputSize, "%s F (last reading)", temperatureText);
  } else {
    snprintf(output, outputSize, "%s F", temperatureText);
  }
}

// Formats a reading held in tenths for a reply, e.g. 713 -> "71.3", or "NA" while the sensor has
// no usable reading (starting, locked out or failed).
void formatReadingOrNA(char *output, size_t outputSize, int32_t readingTenths) {
  bool usable = temperatureUsable();
  if (!usable) {
    snprintf(output, outputSize, "NA");
    return;
  }
  formatTenths(output, outputSize, readingTenths);
}

// Switches cool mode off when the sensor fails or its reading freezes: the AC must not run without
// a temperature. The change is saved like any other, so cool stays off until it is set again, and
// the status keeps reporting why until then (settings.coolShutOff), even after the sensor recovers.
// The cause goes in the log, e.g. "DHT22 failed".
void switchCoolOffForSensor(const char *cause) {
  bool coolSelected = (settings.hvacMode == HVAC_MODE_COOL);
  if (!coolSelected) {
    return;
  }
  settings.hvacMode = (uint8_t)HVAC_MODE_OFF;
  settings.coolShutOff = 1;
  markSettingsChanged();
  logMessage("ERROR %s: cool mode switched off", cause);
}

// Updates sensorState and logs each change: the first reading, the startup lockout, the sensor
// going stale, failing (heat drops to 25%, cool is switched off) and reading again.
void updateSensorState(uint64_t nowMs) {
  int16_t newState = evaluateSensorState(nowMs);
  bool stateChanged = (newState != sensorState);
  if (!stateChanged) {
    return;
  }
  int16_t previousState = sensorState;
  sensorState = newState;
  const char *resultName = dhtResultName(sensorLastResult);
  if (newState == SENSOR_STATE_OK) {
    char temperatureText[TEMPERATURE_TEXT_SIZE];
    char humidityText[TEMPERATURE_TEXT_SIZE];
    formatTenths(temperatureText, sizeof(temperatureText), sensorTemperatureTenthsF);
    formatTenths(humidityText, sizeof(humidityText), sensorHumidityTenths);
    bool wasLockedOut = (previousState == SENSOR_STATE_LOCKOUT);
    bool wasFailing = (previousState == SENSOR_STATE_STALE) || (previousState == SENSOR_STATE_FAILED);
    if (wasLockedOut) {
      logMessage("DHT22 reading: %s F, %s %%, startup lockout cleared", temperatureText, humidityText);
    } else if (wasFailing) {
      logMessage("DHT22 reading again: %s F, %s %%", temperatureText, humidityText);
    } else {
      logMessage("DHT22 reading: %s F, %s %%", temperatureText, humidityText);
    }
  } else if (newState == SENSOR_STATE_LOCKOUT) {
    logMessage("ERROR DHT22 not readable at startup (%s): HVAC locked out", resultName);
  } else if (newState == SENSOR_STATE_STALE) {
    unsigned long staleSeconds = msToSeconds(DHT_STALE_MS);
    logMessage("ERROR DHT22 no reading for %lus (%s), heat/cool paused", staleSeconds, resultName);
  } else if (newState == SENSOR_STATE_FAILED) {
    unsigned long failedSeconds = msToSeconds(DHT_FAILED_MS);
    logMessage("ERROR DHT22 failed, no reading for %lus (%s)", failedSeconds, resultName);
    bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
    if (heatSelected) {
      unsigned long onSeconds = msToSeconds(HEAT_FALLBACK_ON_MS);
      unsigned long periodSeconds = msToSeconds(HEAT_FALLBACK_PERIOD_MS);
      logMessage("ERROR DHT22 failed: heat running %u%% (%lus every %lus)", HEAT_FALLBACK_PERCENT,
                 onSeconds, periodSeconds);
    }
    switchCoolOffForSensor("DHT22 failed");
  }
}

// Returns true while a reading that passes its checks is known to be frozen. A stale or failed
// sensor has its own rules, which take over.
bool readingKnownFrozen() {
  bool readingFresh = (sensorState == SENSOR_STATE_OK);
  bool knownFrozen = frozenReadingActive && readingFresh;
  return knownFrozen;
}

// Returns true while heat runs on the 25% fallback because the sensor has failed or its reading is frozen.
bool heatFallbackActive() {
  bool sensorFailed = (sensorState == SENSOR_STATE_FAILED);
  bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
  bool fallbackActive = (sensorFailed || readingKnownFrozen()) && heatSelected;
  return fallbackActive;
}

// Returns true during the on part of the 25% fallback: the first HEAT_FALLBACK_ON_MS of every
// HEAT_FALLBACK_PERIOD_MS, counted from the moment the sensor failed. With no reading since boot,
// sensorLastValidMs is the boot itself (see initSensor). A frozen reading's fallback starts when its
// rest ends.
bool heatFallbackWantsOn(uint64_t nowMs) {
  uint64_t sensorFailedSinceMs = sensorLastValidMs + DHT_FAILED_MS;
  if (readingKnownFrozen()) {
    sensorFailedSinceMs = frozenFallbackStartMs;
  }
  uint64_t msSinceFailure = msSince(sensorFailedSinceMs, nowMs);
  uint64_t msIntoPeriod = msSinceFailure % HEAT_FALLBACK_PERIOD_MS;
  bool inOnPart = (msIntoPeriod < HEAT_FALLBACK_ON_MS);
  return inOnPart;
}

// Sets up the DHT22's data line (idle high), schedules its first reading DHT_POWER_UP_MS after boot
// and starts the time without a reading from boot.
void initSensor() {
  pinMode(DHT22_DATA_PIN, INPUT_PULLUP);
  sensorNextReadMs = bootMs + DHT_POWER_UP_MS;
  sensorLastValidMs = bootMs;
}

// ==== starts per hour ====

// Drops the starts older than CYCLE_WINDOW_MS; the oldest start is always first.
void pruneStartHistory(StartHistory &history, uint64_t nowMs) {
  uint8_t expiredCount = 0;
  while (expiredCount < history.count) {
    uint64_t startMs = history.startMs[expiredCount];
    bool expired = intervalElapsed(startMs, CYCLE_WINDOW_MS, nowMs);
    if (!expired) {
      break;
    }
    expiredCount = expiredCount + 1;
  }
  if (expiredCount == 0) {
    return;
  }
  uint8_t keptCount = history.count - expiredCount;
  for (uint8_t keptIndex = 0; keptIndex < keptCount; keptIndex++) {
    uint8_t sourceIndex = keptIndex + expiredCount;
    history.startMs[keptIndex] = history.startMs[sourceIndex];
  }
  history.count = keptCount;
}

// Adds a start at nowMs, dropping the oldest when the history is full.
void recordStart(StartHistory &history, uint64_t nowMs) {
  bool historyFull = (history.count >= START_HISTORY_CAPACITY);
  if (historyFull) {
    uint8_t lastIndex = START_HISTORY_CAPACITY - 1;
    for (uint8_t keptIndex = 0; keptIndex < lastIndex; keptIndex++) {
      uint8_t sourceIndex = keptIndex + 1;
      history.startMs[keptIndex] = history.startMs[sourceIndex];
    }
    history.count = lastIndex;
  }
  history.startMs[history.count] = nowMs;
  history.count = history.count + 1;
}

// Returns how long until another start fits within the limit: 0 while fewer than `limit`
// starts are in the last hour, otherwise until the start that fills the limit leaves the window.
uint64_t cycleLimitRemainingMs(const StartHistory &history, uint8_t limit, uint64_t nowMs) {
  bool belowLimit = (history.count < limit);
  if (belowLimit) {
    return 0;
  }
  uint8_t limitingIndex = history.count - limit;
  uint64_t limitingStartMs = history.startMs[limitingIndex];
  uint64_t remainingMs = msRemaining(limitingStartMs, CYCLE_WINDOW_MS, nowMs);
  return remainingMs;
}

// ==== relays and interlocks ====

// Drives one relay: HIGH closes its contact (R to the terminal), LOW opens it.
void writeRelayPin(uint8_t relayPin, bool closed) {
  if (closed) {
    digitalWrite(relayPin, HIGH);
  } else {
    digitalWrite(relayPin, LOW);
  }
}

// Sets one relay pin as an output at 12 mA drive strength and opens its contact. The pad
// defaults to 4 mA; 12 mA holds the pin near 3.3 V while it feeds the 2N5551 driver's base current.
void initRelayPin(uint8_t relayPin) {
  pinMode(relayPin, OUTPUT);
  gpio_set_drive_strength(relayPin, GPIO_DRIVE_STRENGTH_12MA);
  writeRelayPin(relayPin, false);
}

// Sets every relay pin as an output and opens every contact; the first thing setup() does.
void initRelays() {
  initRelayPin(RELAY_HEAT_W1_PIN);
  initRelayPin(RELAY_COOL_Y1_PIN);
  initRelayPin(RELAY_FAN_G_PIN);
  relayState.heatW1 = false;
  relayState.coolY1 = false;
  relayState.fanG = false;
}

// Opens every relay contact at once, without the usual bookkeeping; used just before a restart.
void openAllRelays() {
  writeRelayPin(RELAY_COOL_Y1_PIN, false);
  writeRelayPin(RELAY_HEAT_W1_PIN, false);
  writeRelayPin(RELAY_FAN_G_PIN, false);
}

// Logs an interlock refusal once, until a request passes every interlock again; null means none.
void noteInterlockRefusal(const char *refusal) {
  if (refusal == nullptr) {
    lastInterlockMessage = nullptr;
    return;
  }
  bool alreadyLogged = (refusal == lastInterlockMessage);
  if (alreadyLogged) {
    return;
  }
  interlockRefusals = interlockRefusals + 1;
  lastInterlockMessage = refusal;
  logMessage("ERROR interlock: %s", refusal);
}

// The last line of defence: corrects a relay request that would break a rule from the reference,
// whatever asked for it. The thermostat logic already keeps to these rules, so a refusal here
// means a bug and is logged as an error.
//   - W1 and Y1 are never closed together: both are held open
//   - Y1 may close only with W1 open, 5 min after Y1 last opened (and after power-up) and 5 min
//     after W1 last opened
//   - W1 may close only with Y1 open and 5 min after Y1 last opened
//   - Y1 is never closed without G: G is closed with it
RelayOutputs applyInterlocks(RelayOutputs requested, uint64_t nowMs) {
  RelayOutputs allowed = requested;
  const char *refusal = nullptr;
  bool heatAndCool = allowed.heatW1 && allowed.coolY1;
  if (heatAndCool) {
    allowed.heatW1 = false;
    allowed.coolY1 = false;
    refusal = "W1 and Y1 requested together, both held open";
  }
  bool coolStarting = allowed.coolY1 && !relayState.coolY1;
  if (coolStarting) {
    bool offTimeOver = intervalElapsed(coolLastOffMs, COMPRESSOR_MIN_OFF_MS, nowMs);
    bool deadBandOver = intervalElapsed(heatLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
    bool heatOpen = !relayState.heatW1;
    bool coolMayStart = offTimeOver && deadBandOver && heatOpen;
    if (!coolMayStart) {
      allowed.coolY1 = false;
      refusal = "Y1 start refused: compressor off time or dead band";
    }
  }
  bool heatStarting = allowed.heatW1 && !relayState.heatW1;
  if (heatStarting) {
    bool deadBandOver = intervalElapsed(coolLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
    bool coolOpen = !relayState.coolY1;
    bool heatMayStart = deadBandOver && coolOpen;
    if (!heatMayStart) {
      allowed.heatW1 = false;
      refusal = "W1 start refused: dead band";
    }
  }
  bool coolWithoutFan = allowed.coolY1 && !allowed.fanG;
  if (coolWithoutFan) {
    allowed.fanG = true;
    refusal = "Y1 requested without G, G closed";
  }
  noteInterlockRefusal(refusal);
  return allowed;
}

// Records the timers and start counts for every relay that changed and logs each change with the
// control temperature, e.g. "heat off (W1 opened) after 612s: setpoint reached, 70.5 F".
void recordRelayChanges(RelayOutputs previous, RelayOutputs current, uint64_t nowMs) {
  char temperatureText[TEMPERATURE_TEXT_SIZE];
  formatControlTemperature(temperatureText, sizeof(temperatureText));
  bool heatStarted = !previous.heatW1 && current.heatW1;
  bool heatStopped = previous.heatW1 && !current.heatW1;
  bool coolStarted = !previous.coolY1 && current.coolY1;
  bool coolStopped = previous.coolY1 && !current.coolY1;
  bool fanStarted = !previous.fanG && current.fanG;
  bool fanStopped = previous.fanG && !current.fanG;
  if (heatStarted) {
    heatOnSinceMs = nowMs;
    recordStart(heatStarts, nowMs);
    heatMinOnHoldLogged = false;
    bool onFallback = heatFallbackActive();
    if (onFallback) {
      const char *fallbackCause = "sensor failed";
      if (readingKnownFrozen()) {
        fallbackCause = "reading frozen";
      }
      logMessage("heat on (W1 closed): %s, %u%% fallback", fallbackCause, HEAT_FALLBACK_PERCENT);
    } else {
      logMessage("heat on (W1 closed): %s, setpoint %u F", temperatureText, settings.setpointF);
    }
  }
  if (heatStopped) {
    heatLastOffMs = nowMs;
    unsigned long secondsOn = msToSeconds(msSince(heatOnSinceMs, nowMs));
    logMessage("heat off (W1 opened) after %lus: %s, %s", secondsOn, heatChangeReason, temperatureText);
  }
  if (coolStarted) {
    coolOnSinceMs = nowMs;
    recordStart(coolStarts, nowMs);
    coolMinOnHoldLogged = false;
    const char *relaysClosed = "Y1 closed";
    if (fanStarted) {
      relaysClosed = "G, Y1 closed";
    }
    logMessage("cool on (%s): %s, setpoint %u F", relaysClosed, temperatureText, settings.setpointF);
  }
  if (coolStopped) {
    coolLastOffMs = nowMs;
    unsigned long secondsOn = msToSeconds(msSince(coolOnSinceMs, nowMs));
    const char *relaysOpened = "Y1 opened";
    if (fanStopped) {
      relaysOpened = "Y1, G opened";
    }
    logMessage("cool off (%s) after %lus: %s, %s", relaysOpened, secondsOn, coolChangeReason, temperatureText);
  }
  bool coolChanged = coolStarted || coolStopped;
  bool fanChangedAlone = (fanStarted || fanStopped) && !coolChanged;
  if (fanChangedAlone) {
    const char *fanChange = "fan off (G opened)";
    if (fanStarted) {
      fanChange = "fan on (G closed)";
    }
    logMessage("%s: %s", fanChange, fanChangeReason);
  }
}

// Brings the relays to the requested outputs, through the interlocks. Contacts open before any
// close, so W1 and Y1 are never closed at the same moment; G closes before Y1 and Y1 opens
// before G, so the compressor never runs without the blower.
void driveRelays(RelayOutputs requested, uint64_t nowMs) {
  RelayOutputs allowed = applyInterlocks(requested, nowMs);
  RelayOutputs previous = relayState;
  bool coolOpens = previous.coolY1 && !allowed.coolY1;
  bool heatOpens = previous.heatW1 && !allowed.heatW1;
  bool fanOpens = previous.fanG && !allowed.fanG;
  bool fanCloses = !previous.fanG && allowed.fanG;
  bool heatCloses = !previous.heatW1 && allowed.heatW1;
  bool coolCloses = !previous.coolY1 && allowed.coolY1;
  if (coolOpens) {
    writeRelayPin(RELAY_COOL_Y1_PIN, false);
  }
  if (heatOpens) {
    writeRelayPin(RELAY_HEAT_W1_PIN, false);
  }
  if (fanOpens) {
    writeRelayPin(RELAY_FAN_G_PIN, false);
  }
  if (fanCloses) {
    writeRelayPin(RELAY_FAN_G_PIN, true);
  }
  if (heatCloses) {
    writeRelayPin(RELAY_HEAT_W1_PIN, true);
  }
  if (coolCloses) {
    writeRelayPin(RELAY_COOL_Y1_PIN, true);
  }
  relayState = allowed;
  recordRelayChanges(previous, allowed, nowMs);
}

// Returns what the equipment is doing from the relays: heating (W1), cooling (Y1), fan only (G) or idle.
int16_t runningState() {
  if (relayState.heatW1) {
    return RUNNING_HEATING;
  }
  if (relayState.coolY1) {
    return RUNNING_COOLING;
  }
  if (relayState.fanG) {
    return RUNNING_FAN_ONLY;
  }
  return RUNNING_IDLE;
}

// ==== thermostat ====

// Returns the setpoint in tenths of a degree F, e.g. 70 F -> 700.
int32_t setpointTenthsF() {
  int32_t setpointWholeDegrees = settings.setpointF;
  int32_t setpointTenths = setpointWholeDegrees * TENTHS_PER_DEGREE;
  return setpointTenths;
}

// Heat starts at or below the setpoint minus half the swing, e.g. 69.5 F at a 70 F setpoint.
int32_t heatStartTenthsF() {
  int32_t startTenths = setpointTenthsF() - HALF_SWING_TENTHS_F;
  return startTenths;
}

// Heat stops at or above the setpoint plus half the swing, e.g. 70.5 F.
int32_t heatStopTenthsF() {
  int32_t stopTenths = setpointTenthsF() + HALF_SWING_TENTHS_F;
  return stopTenths;
}

// Cooling starts at or above the setpoint plus half the swing, e.g. 70.5 F.
int32_t coolStartTenthsF() {
  int32_t startTenths = setpointTenthsF() + HALF_SWING_TENTHS_F;
  return startTenths;
}

// Cooling stops at or below the setpoint minus half the swing, e.g. 69.5 F.
int32_t coolStopTenthsF() {
  int32_t stopTenths = setpointTenthsF() - HALF_SWING_TENTHS_F;
  return stopTenths;
}

// Returns what keeps a wanted heat start waiting, or BLOCKER_NONE when W1 may close now. A relay
// hold (sensor lockout) comes first.
StartBlocker heatStartBlocker(uint64_t nowMs, StartBlocker relayHold) {
  bool holdActive = (relayHold != BLOCKER_NONE);
  if (holdActive) {
    return relayHold;
  }
  bool powerUpOver = intervalElapsed(bootMs, POWER_UP_DELAY_MS, nowMs);
  if (!powerUpOver) {
    return BLOCKER_POWER_UP;
  }
  if (sensorState == SENSOR_STATE_STARTING) {
    return BLOCKER_SENSOR_STARTING;
  }
  if (sensorState == SENSOR_STATE_STALE) {
    return BLOCKER_SENSOR_STALE;
  }
  if (runLimitResting) {
    return BLOCKER_RUN_LIMIT_REST;
  }
  bool deadBandOver = intervalElapsed(coolLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
  bool coolRunning = relayState.coolY1;
  if (coolRunning || !deadBandOver) {
    return BLOCKER_CHANGEOVER;
  }
  bool limitReached = (heatStarts.count >= HEAT_STARTS_PER_HOUR_MAX);
  if (limitReached) {
    return BLOCKER_CYCLE_LIMIT;
  }
  return BLOCKER_NONE;
}

// Returns what keeps a wanted cool start waiting, or BLOCKER_NONE when Y1 may close now. A relay
// hold (sensor lockout) comes first.
StartBlocker coolStartBlocker(uint64_t nowMs, StartBlocker relayHold) {
  bool holdActive = (relayHold != BLOCKER_NONE);
  if (holdActive) {
    return relayHold;
  }
  bool powerUpOver = intervalElapsed(bootMs, POWER_UP_DELAY_MS, nowMs);
  if (!powerUpOver) {
    return BLOCKER_POWER_UP;
  }
  if (sensorState == SENSOR_STATE_STARTING) {
    return BLOCKER_SENSOR_STARTING;
  }
  if (sensorState == SENSOR_STATE_STALE) {
    return BLOCKER_SENSOR_STALE;
  }
  if (runLimitResting) {
    return BLOCKER_RUN_LIMIT_REST;
  }
  bool deadBandOver = intervalElapsed(heatLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
  bool heatRunning = relayState.heatW1;
  if (heatRunning || !deadBandOver) {
    return BLOCKER_CHANGEOVER;
  }
  bool offTimeOver = intervalElapsed(coolLastOffMs, COMPRESSOR_MIN_OFF_MS, nowMs);
  if (!offTimeOver) {
    return BLOCKER_COMPRESSOR_OFF_TIME;
  }
  bool limitReached = (coolStarts.count >= COOL_STARTS_PER_HOUR_MAX);
  if (limitReached) {
    return BLOCKER_CYCLE_LIMIT;
  }
  return BLOCKER_NONE;
}

// Returns how long a blocker still holds a start, or 0 when it has no fixed end (Wi-Fi, the
// sensor). forHeat picks the heat stage's timers, otherwise the cool stage's.
uint64_t startBlockerRemainingMs(StartBlocker blocker, bool forHeat, uint64_t nowMs) {
  switch (blocker) {
    case BLOCKER_POWER_UP:
      return msRemaining(bootMs, POWER_UP_DELAY_MS, nowMs);
    case BLOCKER_COMPRESSOR_OFF_TIME:
      return msRemaining(coolLastOffMs, COMPRESSOR_MIN_OFF_MS, nowMs);
    case BLOCKER_RUN_LIMIT_REST:
      return msRemaining(runLimitRestStartMs, RUN_LIMIT_REST_MS, nowMs);
    case BLOCKER_CHANGEOVER:
      if (forHeat) {
        if (relayState.coolY1) {
          return CHANGEOVER_DEAD_BAND_MS; // starts once cooling has stopped
        }
        return msRemaining(coolLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
      }
      if (relayState.heatW1) {
        return CHANGEOVER_DEAD_BAND_MS; // starts once heating has stopped
      }
      return msRemaining(heatLastOffMs, CHANGEOVER_DEAD_BAND_MS, nowMs);
    case BLOCKER_CYCLE_LIMIT:
      if (forHeat) {
        return cycleLimitRemainingMs(heatStarts, HEAT_STARTS_PER_HOUR_MAX, nowMs);
      }
      return cycleLimitRemainingMs(coolStarts, COOL_STARTS_PER_HOUR_MAX, nowMs);
    default:
      return 0;
  }
}

// Records why a wanted start waits, logging each new reason once, e.g.
// "cool call waiting: compressor off time, 240s left".
void noteStartBlocker(const char *stageName, StartBlocker &currentBlocker, StartBlocker newBlocker, bool forHeat, uint64_t nowMs) {
  bool blockerChanged = (newBlocker != currentBlocker);
  currentBlocker = newBlocker;
  bool waiting = (newBlocker != BLOCKER_NONE);
  if (!blockerChanged || !waiting) {
    return;
  }
  const char *blockerName = startBlockerName(newBlocker);
  uint64_t remainingMs = startBlockerRemainingMs(newBlocker, forHeat, nowMs);
  if (remainingMs > 0) {
    unsigned long secondsLeft = msToSecondsRoundedUp(remainingMs);
    logMessage("%s call waiting: %s, %lus left", stageName, blockerName, secondsLeft);
  } else {
    logMessage("%s call waiting: %s", stageName, blockerName);
  }
}

// Returns why every relay must be held open this pass: BLOCKER_SENSOR_LOCKOUT while the sensor
// hasn't read since boot, otherwise BLOCKER_NONE. Wi-Fi never holds the relays: the HVAC keeps
// working without it.
StartBlocker relayHoldReason() {
  bool sensorLockout = (sensorState == SENSOR_STATE_LOCKOUT);
  if (sensorLockout) {
    return BLOCKER_SENSOR_LOCKOUT;
  }
  return BLOCKER_NONE;
}

// Decides whether W1 stays closed. A relay hold (sensor lockout), off and a run limit open it at
// once (a run limit is always past the minimum on time). Otherwise it opens once the furnace
// minimum on time has run and the stop temperature is reached, the mode has left heat, the sensor
// has gone stale (the furnace never runs on an old reading), or (sensor failed or reading frozen)
// the 25% fallback's on part has ended.
bool keepHeating(uint64_t nowMs, StartBlocker relayHold) {
  bool holdActive = (relayHold != BLOCKER_NONE);
  if (holdActive) {
    heatChangeReason = startBlockerName(relayHold);
    return false;
  }
  bool modeOff = (settings.hvacMode == HVAC_MODE_OFF);
  if (modeOff) {
    heatChangeReason = "mode off";
    return false;
  }
  if (runLimitStopDue) {
    heatChangeReason = runLimitStopReason;
    return false;
  }
  bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
  bool onFallback = heatFallbackActive();
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  bool usable = temperatureUsable();
  bool wantsStop = !heatSelected;
  const char *stopReason = "mode changed";
  if (heatSelected && onFallback) {
    bool fallbackOn = heatFallbackWantsOn(nowMs);
    wantsStop = !fallbackOn;
    stopReason = "fallback off time";
  } else if (heatSelected && sensorStale) {
    wantsStop = true;
    stopReason = "sensor stale";
  } else if (heatSelected && usable) {
    int32_t temperatureTenthsF = controlTemperatureTenthsF();
    int32_t stopTenths = heatStopTenthsF();
    wantsStop = (temperatureTenthsF >= stopTenths);
    stopReason = "setpoint reached";
  }
  if (!wantsStop) {
    return true;
  }
  bool minOnTimeOver = intervalElapsed(heatOnSinceMs, FURNACE_MIN_ON_MS, nowMs);
  if (!minOnTimeOver) {
    if (!heatMinOnHoldLogged) {
      uint64_t remainingMs = msRemaining(heatOnSinceMs, FURNACE_MIN_ON_MS, nowMs);
      unsigned long secondsLeft = msToSecondsRoundedUp(remainingMs);
      logMessage("heat %s, held for furnace minimum on time, %lus left", stopReason, secondsLeft);
      heatMinOnHoldLogged = true;
    }
    return true;
  }
  heatChangeReason = stopReason;
  return false;
}

// Decides whether W1 closes, in heat mode once nothing blocks it: at or below the start
// temperature, or during the on part of the 25% fallback when the sensor has failed. While the
// sensor is starting or locked out the call can't be known, so it shows as waiting for the sensor;
// while stale the last reading's call waits for a fresh one (sensor stale blocker).
bool startHeating(uint64_t nowMs, StartBlocker relayHold) {
  bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
  if (!heatSelected) {
    heatBlocker = BLOCKER_NONE;
    return false;
  }
  bool callWanted = true;
  bool onFallback = heatFallbackActive();
  bool usable = temperatureUsable();
  if (onFallback) {
    callWanted = heatFallbackWantsOn(nowMs);
  } else if (usable) {
    int32_t temperatureTenthsF = controlTemperatureTenthsF();
    int32_t startTenths = heatStartTenthsF();
    callWanted = (temperatureTenthsF <= startTenths);
  }
  if (!callWanted) {
    heatBlocker = BLOCKER_NONE;
    return false;
  }
  StartBlocker blocker = heatStartBlocker(nowMs, relayHold);
  noteStartBlocker("heat", heatBlocker, blocker, true, nowMs);
  bool mayStart = (blocker == BLOCKER_NONE);
  return mayStart;
}

// Returns whether W1 should be closed this pass.
bool decideHeatRelay(uint64_t nowMs, StartBlocker relayHold) {
  if (relayState.heatW1) {
    heatBlocker = BLOCKER_NONE;
    return keepHeating(nowMs, relayHold);
  }
  return startHeating(nowMs, relayHold);
}

// Decides whether Y1 stays closed. A relay hold, a failed sensor and off open it at once (the
// reference allows breaking the minimum on time only for safety) with no blower run-on. A run limit
// opens it at once (always past the minimum on time) and lets G run on. Reaching the stop
// temperature, leaving cool mode or the sensor going stale (the compressor never runs on an old
// reading) opens it once the compressor minimum on time has run, and lets G run on.
bool keepCooling(uint64_t nowMs, StartBlocker relayHold) {
  bool holdActive = (relayHold != BLOCKER_NONE);
  if (holdActive) {
    coolChangeReason = startBlockerName(relayHold);
    coolRunOnAllowed = false;
    return false;
  }
  bool sensorFailed = (sensorState == SENSOR_STATE_FAILED);
  if (sensorFailed) {
    coolChangeReason = "sensor failed";
    coolRunOnAllowed = false;
    return false;
  }
  bool modeOff = (settings.hvacMode == HVAC_MODE_OFF);
  if (modeOff) {
    coolChangeReason = "mode off";
    coolRunOnAllowed = false;
    return false;
  }
  if (runLimitStopDue) {
    coolChangeReason = runLimitStopReason;
    coolRunOnAllowed = true;
    return false;
  }
  bool coolSelected = (settings.hvacMode == HVAC_MODE_COOL);
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  bool wantsStop = !coolSelected;
  const char *stopReason = "mode changed";
  if (coolSelected && sensorStale) {
    wantsStop = true;
    stopReason = "sensor stale";
  } else if (coolSelected) {
    // Y1 only runs on a fresh reading: starting, lockout and stale never close it, failed opens it above
    int32_t temperatureTenthsF = controlTemperatureTenthsF();
    int32_t stopTenths = coolStopTenthsF();
    wantsStop = (temperatureTenthsF <= stopTenths);
    stopReason = "setpoint reached";
  }
  if (!wantsStop) {
    return true;
  }
  bool minOnTimeOver = intervalElapsed(coolOnSinceMs, COMPRESSOR_MIN_ON_MS, nowMs);
  if (!minOnTimeOver) {
    if (!coolMinOnHoldLogged) {
      uint64_t remainingMs = msRemaining(coolOnSinceMs, COMPRESSOR_MIN_ON_MS, nowMs);
      unsigned long secondsLeft = msToSecondsRoundedUp(remainingMs);
      logMessage("cool %s, held for compressor minimum on time, %lus left", stopReason, secondsLeft);
      coolMinOnHoldLogged = true;
    }
    return true;
  }
  coolChangeReason = stopReason;
  coolRunOnAllowed = true;
  return false;
}

// Decides whether Y1 closes: in cool mode at or above the start temperature, once nothing blocks
// it; never with a failed sensor. While the sensor is starting or locked out the call can't be
// known, so it shows as waiting for the sensor; while stale the last reading's call waits for a
// fresh one (sensor stale blocker).
bool startCooling(uint64_t nowMs, StartBlocker relayHold) {
  bool coolSelected = (settings.hvacMode == HVAC_MODE_COOL);
  bool sensorFailed = (sensorState == SENSOR_STATE_FAILED);
  bool coolMayBeCalled = coolSelected && !sensorFailed;
  if (!coolMayBeCalled) {
    coolBlocker = BLOCKER_NONE;
    return false;
  }
  bool callWanted = true;
  bool usable = temperatureUsable();
  if (usable) {
    int32_t temperatureTenthsF = controlTemperatureTenthsF();
    int32_t startTenths = coolStartTenthsF();
    callWanted = (temperatureTenthsF >= startTenths);
  }
  if (!callWanted) {
    coolBlocker = BLOCKER_NONE;
    return false;
  }
  StartBlocker blocker = coolStartBlocker(nowMs, relayHold);
  noteStartBlocker("cool", coolBlocker, blocker, false, nowMs);
  bool mayStart = (blocker == BLOCKER_NONE);
  return mayStart;
}

// Returns whether Y1 should be closed this pass.
bool decideCoolRelay(uint64_t nowMs, StartBlocker relayHold) {
  if (relayState.coolY1) {
    coolBlocker = BLOCKER_NONE;
    return keepCooling(nowMs, relayHold);
  }
  return startCooling(nowMs, relayHold);
}

// Returns whether G should be closed this pass: always with Y1, always in fan on mode (also with
// heat, which the reference allows, and in off mode as fan only), and for COOL_FAN_RUN_ON_MS after
// a cool cycle ends normally. On heat with fan auto G stays open: the furnace runs its own blower.
// Every relay opens during a relay hold (sensor lockout).
bool decideFanRelay(uint64_t nowMs, bool coolRequested, StartBlocker relayHold) {
  bool holdActive = (relayHold != BLOCKER_NONE);
  if (holdActive) {
    fanChangeReason = startBlockerName(relayHold);
    return false;
  }
  if (coolRequested) {
    fanChangeReason = "cooling";
    return true;
  }
  bool fanModeOn = (settings.fanMode == FAN_MODE_ON);
  if (fanModeOn) {
    fanChangeReason = "fan mode on";
    return true;
  }
  // Y1 opening this pass hasn't stamped coolLastOffMs yet, but its run-on starts now
  bool coolStoppingNow = relayState.coolY1 && !coolRequested;
  bool runOnTimeLeft = !intervalElapsed(coolLastOffMs, COOL_FAN_RUN_ON_MS, nowMs);
  bool withinRunOn = coolStoppingNow || runOnTimeLeft;
  bool modeOff = (settings.hvacMode == HVAC_MODE_OFF);
  bool runOn = coolRunOnAllowed && withinRunOn && !modeOff;
  if (runOn) {
    fanChangeReason = "cool run-on";
    return true;
  }
  if (relayState.fanG) {
    fanChangeReason = "fan auto, idle";
  }
  return false;
}

// Marks the running stage to open this pass and starts its rest.
void stopStageForRunLimit(const char *reason, const char *stageName, uint64_t nowMs) {
  runLimitStopDue = true;
  runLimitStopReason = reason;
  runLimitStageName = stageName;
  runLimitResting = true;
  runLimitRestStartMs = nowMs;
  responseTracking = false;
}

// Counts a no-response stop towards the frozen reading: FROZEN_READING_STOPS of them, with the
// reading not moving RESPONSE_MIN_CHANGE_TENTHS_F from where it stood at the first, is the last
// step. Heat drops to the 25% fallback once this stop's rest ends, and cool switches off.
void countFrozenReadingStop(bool heating, uint64_t nowMs) {
  if (frozenStopCount == 0) {
    frozenReferenceTenthsF = sensorTemperatureTenthsF;
  }
  frozenStopCount = frozenStopCount + 1;
  if (frozenStopCount < FROZEN_READING_STOPS) {
    return;
  }
  frozenStopCount = 0;
  if (heating) {
    frozenReadingActive = true;
    frozenFallbackStartMs = nowMs + RUN_LIMIT_REST_MS;
    logMessage("ERROR reading frozen: heat running %u%% until it moves", HEAT_FALLBACK_PERCENT);
  } else {
    switchCoolOffForSensor("reading frozen");
  }
}

// Forgets the frozen-reading count and ends the 25% fallback once a fresh reading has moved
// RESPONSE_MIN_CHANGE_TENTHS_F either way from where it stood: the sensor is working after all.
void checkReadingStillFrozen() {
  bool watching = frozenReadingActive || (frozenStopCount > 0);
  bool readingFresh = (sensorState == SENSOR_STATE_OK);
  if (!watching || !readingFresh) {
    return;
  }
  int32_t movedTenthsF = sensorTemperatureTenthsF - frozenReferenceTenthsF;
  bool movedUp = (movedTenthsF >= RESPONSE_MIN_CHANGE_TENTHS_F);
  bool movedDown = (movedTenthsF <= -RESPONSE_MIN_CHANGE_TENTHS_F);
  if (!movedUp && !movedDown) {
    return;
  }
  frozenStopCount = 0;
  if (frozenReadingActive) {
    frozenReadingActive = false;
    noResponseAlarm = false;
    logMessage("reading moving again: heat back to normal control");
  }
}

// Limits how long W1 and Y1 run, whatever asks for them. Called before the relays are decided.
//   - No response: W1 for HEAT_RESPONSE_WINDOW_MS, or Y1 for COOL_RESPONSE_WINDOW_MS, without the
//     room moving the right way by RESPONSE_MIN_CHANGE_TENTHS_F on fresh readings (a stuck sensor,
//     a furnace lockout, a tripped AC breaker, or equipment that can't keep up).
//   - Hard cap: W1 for HEAT_MAX_RUN_MS or Y1 for COOL_MAX_RUN_MS in one run, even while it is improving.
// Either opens the stage and rests both stages for RUN_LIMIT_REST_MS, and then the call may run
// again. A working sensor shows the room drifting during the rest, which resets the count, so only
// a frozen reading reaches the last step (see countFrozenReadingStop).
void updateRunLimits(uint64_t nowMs) {
  runLimitStopDue = false;
  if (runLimitResting) {
    bool restOver = intervalElapsed(runLimitRestStartMs, RUN_LIMIT_REST_MS, nowMs);
    if (restOver) {
      runLimitResting = false;
    }
  }
  checkReadingStillFrozen();
  bool heating = relayState.heatW1;
  bool cooling = relayState.coolY1;
  if (!heating && !cooling) {
    responseTracking = false;
    return;
  }
  const char *stageName = "cool";
  uint64_t onSinceMs = coolOnSinceMs;
  uint64_t maxRunMs = COOL_MAX_RUN_MS;
  uint64_t windowMs = COOL_RESPONSE_WINDOW_MS;
  int32_t changeTenthsF = responseCheckpointTenthsF - sensorTemperatureTenthsF;
  if (heating) {
    stageName = "heat";
    onSinceMs = heatOnSinceMs;
    maxRunMs = HEAT_MAX_RUN_MS;
    windowMs = HEAT_RESPONSE_WINDOW_MS;
    changeTenthsF = sensorTemperatureTenthsF - responseCheckpointTenthsF;
  }
  unsigned long restSeconds = msToSeconds(RUN_LIMIT_REST_MS);
  bool capReached = intervalElapsed(onSinceMs, maxRunMs, nowMs);
  if (capReached) {
    unsigned long capSeconds = msToSeconds(maxRunMs);
    logMessage("ERROR %s: run limit of %lus reached, resting %lus", stageName, capSeconds, restSeconds);
    stopStageForRunLimit("run limit", stageName, nowMs);
    return;
  }
  bool readingFresh = (sensorState == SENSOR_STATE_OK);
  if (!readingFresh) {
    responseTracking = false;
    return;
  }
  if (!responseTracking) {
    responseTracking = true;
    responseCheckpointMs = nowMs;
    responseCheckpointTenthsF = sensorTemperatureTenthsF;
    return;
  }
  bool roomResponded = (changeTenthsF >= RESPONSE_MIN_CHANGE_TENTHS_F);
  if (roomResponded) {
    responseCheckpointMs = nowMs;
    responseCheckpointTenthsF = sensorTemperatureTenthsF;
    noResponseAlarm = false;
    return;
  }
  bool windowOver = intervalElapsed(responseCheckpointMs, windowMs, nowMs);
  if (!windowOver) {
    return;
  }
  unsigned long windowSeconds = msToSeconds(windowMs);
  logMessage("ERROR %s: no temperature response for %lus, resting %lus", stageName, windowSeconds, restSeconds);
  noResponseAlarm = true;
  stopStageForRunLimit("no temperature response", stageName, nowMs);
  countFrozenReadingStop(heating, nowMs);
}

// Forgets every run limit in force: the rest, the frozen-reading count and fallback, and the alarm.
// The stage's hard cap still counts from when it closed. Called when a person sets the hvac mode.
void clearRunLimits() {
  runLimitStopDue = false;
  runLimitResting = false;
  frozenStopCount = 0;
  frozenReadingActive = false;
  noResponseAlarm = false;
  responseTracking = false;
}

// Runs the thermostat once: refreshes the sensor state and start counts, decides each relay and
// drives them through the interlocks; called from core 1 with stateMutex held.
void updateThermostat() {
  uint64_t nowMs = uptimeMs();
  updateSensorState(nowMs);
  pruneStartHistory(heatStarts, nowMs);
  pruneStartHistory(coolStarts, nowMs);
  StartBlocker relayHold = relayHoldReason();
  updateRunLimits(nowMs);
  RelayOutputs requested;
  requested.heatW1 = decideHeatRelay(nowMs, relayHold);
  requested.coolY1 = decideCoolRelay(nowMs, relayHold);
  requested.fanG = decideFanRelay(nowMs, requested.coolY1, relayHold);
  driveRelays(requested, nowMs);
}

// Starts the thermostat's timers at boot (power-up counts as heat and cool both just stopping,
// so the compressor off time and the dead band run from boot) and prepares the sensor; called
// from setup() after the settings are loaded.
void initThermostat() {
  heatLastOffMs = bootMs;
  coolLastOffMs = bootMs;
  heatOnSinceMs = bootMs;
  coolOnSinceMs = bootMs;
  initSensor();
}

// ==== state for the API routes ====

// Returns true until a heat or cool cycle has fully finished: W1 or Y1 closed, or G still in its
// run-on after a cool cycle (the coil is still cold, so the blower must not be cut short).
bool hvacCycleRunning() {
  bool heatOrCoolClosed = relayState.heatW1 || relayState.coolY1;
  uint64_t nowMs = uptimeMs();
  bool runOnTimeLeft = !intervalElapsed(coolLastOffMs, COOL_FAN_RUN_ON_MS, nowMs);
  bool blowerRunningOn = relayState.fanG && coolRunOnAllowed && runOnTimeLeft;
  bool cycleRunning = heatOrCoolClosed || blowerRunningOn;
  return cycleRunning;
}

// Returns what the relays drive now.
RelayOutputs currentRelays() {
  return relayState;
}

// Returns the sensor state, SENSOR_STATE_OK to SENSOR_STATE_FAILED.
int16_t currentSensorState() {
  return sensorState;
}

// Formats the control temperature for a reply, e.g. "71.3" (the last reading while stale), or
// "NA" while there is no usable reading.
void formatTemperatureReading(char *output, size_t outputSize) {
  int32_t temperatureTenthsF = controlTemperatureTenthsF();
  formatReadingOrNA(output, outputSize, temperatureTenthsF);
}

// Formats the humidity for a reply, e.g. "45.2", or "NA" while there is no usable reading.
void formatHumidityReading(char *output, size_t outputSize) {
  formatReadingOrNA(output, outputSize, sensorHumidityTenths);
}

// Sets wholeDegreesF to the control temperature rounded to whole degrees F and returns true, e.g.
// 713 -> 71, 715 -> 72, -45 -> -5, or returns false while there is no usable reading.
bool readWholeTemperatureF(int16_t &wholeDegreesF) {
  bool usable = temperatureUsable();
  if (!usable) {
    return false;
  }
  int32_t temperatureTenthsF = controlTemperatureTenthsF();
  int32_t roundingTenths = TENTHS_PER_DEGREE / 2;
  bool belowZero = (temperatureTenthsF < 0);
  if (belowZero) {
    roundingTenths = -roundingTenths;
  }
  int32_t roundedTenths = temperatureTenthsF + roundingTenths;
  int32_t roundedDegrees = roundedTenths / TENTHS_PER_DEGREE;
  wholeDegreesF = (int16_t)roundedDegrees;
  return true;
}

// Returns how many DHT22 samples have been taken since boot, valid or not. It changes once every
// DHT_READ_INTERVAL_MS, which is how the display knows when to show the temperature again.
uint32_t sensorSampleCount() {
  uint32_t sampleCount = sensorReadsValid + sensorReadsFailed;
  return sampleCount;
}

// ==== status reply sections ====

// Appends the temperature, humidity and sensor lines.
void appendSensorStatus(uint64_t nowMs) {
  bool usable = temperatureUsable();
  if (usable) {
    char temperatureText[TEMPERATURE_TEXT_SIZE];
    char humidityText[TEMPERATURE_TEXT_SIZE];
    formatControlTemperature(temperatureText, sizeof(temperatureText));
    formatTenths(humidityText, sizeof(humidityText), sensorHumidityTenths);
    appendBodyField("temperature:", "%s\n", temperatureText);
    appendBodyField("humidity:", "%s %%\n", humidityText);
  } else {
    appendBodyField("temperature:", "NA\n");
    appendBodyField("humidity:", "NA\n");
  }
  unsigned long readsValid = sensorReadsValid;
  unsigned long readsFailed = sensorReadsFailed;
  const char *stateName = sensorStateName(sensorState);
  if (sensorHasReading) {
    unsigned long readingAgeSeconds = msToSeconds(msSince(sensorLastValidMs, nowMs));
    appendBodyField("sensor:", "%s, last reading %lus ago, %lu ok, %lu failed\n",
                    stateName, readingAgeSeconds, readsValid, readsFailed);
  } else {
    appendBodyField("sensor:", "%s, no reading since boot, %lu failed\n", stateName, readsFailed);
  }
  bool lastReadFailed = (sensorLastResult != DHT_RESULT_OK);
  if (lastReadFailed) {
    const char *resultName = dhtResultName(sensorLastResult);
    unsigned long failuresInRow = sensorConsecutiveFailures;
    appendBodyField("  last read:", "%s (%lu in a row)\n", resultName, failuresInRow);
  }
}

// Appends the setpoint and the temperatures the current mode switches at, or "not used" while
// heat runs the 25% fallback.
void appendSetpointStatus() {
  char lowText[TEMPERATURE_TEXT_SIZE];
  char highText[TEMPERATURE_TEXT_SIZE];
  formatTenths(lowText, sizeof(lowText), heatStartTenthsF());
  formatTenths(highText, sizeof(highText), heatStopTenthsF());
  bool onFallback = heatFallbackActive();
  if (onFallback) {
    appendBodyField("setpoint:", "%u F (not used: sensor failed, heat runs %u%%)\n", settings.setpointF, HEAT_FALLBACK_PERCENT);
  } else if (settings.hvacMode == HVAC_MODE_HEAT) {
    appendBodyField("setpoint:", "%u F (heat on at %s F, off at %s F)\n", settings.setpointF, lowText, highText);
  } else if (settings.hvacMode == HVAC_MODE_COOL) {
    appendBodyField("setpoint:", "%u F (cool on at %s F, off at %s F)\n", settings.setpointF, highText, lowText);
  } else {
    appendBodyField("setpoint:", "%u F\n", settings.setpointF);
  }
}

// Appends one stage's line: running time and the minimum on time left, the reason a wanted
// start waits, or how long it has been off.
void appendStageStatus(const char *label, bool running, uint64_t onSinceMs, uint64_t lastOffMs,
                       uint64_t minOnMs, StartBlocker blocker, bool forHeat, uint64_t nowMs) {
  if (running) {
    unsigned long secondsOn = msToSeconds(msSince(onSinceMs, nowMs));
    uint64_t minOnLeftMs = msRemaining(onSinceMs, minOnMs, nowMs);
    if (minOnLeftMs > 0) {
      unsigned long secondsLeft = msToSecondsRoundedUp(minOnLeftMs);
      appendBodyField(label, "on %lus, minimum on time %lus left\n", secondsOn, secondsLeft);
    } else {
      appendBodyField(label, "on %lus\n", secondsOn);
    }
    return;
  }
  unsigned long secondsOff = msToSeconds(msSince(lastOffMs, nowMs));
  bool waiting = (blocker != BLOCKER_NONE);
  if (!waiting) {
    appendBodyField(label, "off %lus\n", secondsOff);
    return;
  }
  const char *blockerName = startBlockerName(blocker);
  uint64_t remainingMs = startBlockerRemainingMs(blocker, forHeat, nowMs);
  if (remainingMs > 0) {
    unsigned long secondsLeft = msToSecondsRoundedUp(remainingMs);
    appendBodyField(label, "off %lus, call waiting: %s, %lus left\n", secondsOff, blockerName, secondsLeft);
  } else {
    appendBodyField(label, "off %lus, call waiting: %s\n", secondsOff, blockerName);
  }
}

// Returns "closed" or "open" for a relay contact.
const char *contactName(bool closed) {
  if (closed) {
    return "closed";
  }
  return "open";
}

// Appends what is running, the relays, both stages, the compressor lockout and the starts per hour.
void appendEquipmentStatus(uint64_t nowMs) {
  const char *runningName = runningStateName(runningState());
  const char *heatContact = contactName(relayState.heatW1);
  const char *coolContact = contactName(relayState.coolY1);
  const char *fanContact = contactName(relayState.fanG);
  appendBodyField("running:", "%s\n", runningName);
  appendBodyField("relays:", "W1 %s, Y1 %s, G %s\n", heatContact, coolContact, fanContact);
  appendStageStatus("heat:", relayState.heatW1, heatOnSinceMs, heatLastOffMs, FURNACE_MIN_ON_MS, heatBlocker, true, nowMs);
  appendStageStatus("cool:", relayState.coolY1, coolOnSinceMs, coolLastOffMs, COMPRESSOR_MIN_ON_MS, coolBlocker, false, nowMs);
  uint64_t lockoutLeftMs = msRemaining(coolLastOffMs, COMPRESSOR_MIN_OFF_MS, nowMs);
  if (relayState.coolY1) {
    appendBodyField("compressor:", "running\n");
  } else if (lockoutLeftMs > 0) {
    unsigned long lockoutSecondsLeft = msToSecondsRoundedUp(lockoutLeftMs);
    appendBodyField("compressor:", "off-time lockout, %lus left\n", lockoutSecondsLeft);
  } else {
    appendBodyField("compressor:", "ready\n");
  }
  appendBodyField("starts last hour:", "heat %u of %u, cool %u of %u\n",
                  heatStarts.count, HEAT_STARTS_PER_HOUR_MAX, coolStarts.count, COOL_STARTS_PER_HOUR_MAX);
}

// Returns true while the sensor is locked out, stale or failed.
bool sensorHasError() {
  bool sensorLockout = (sensorState == SENSOR_STATE_LOCKOUT);
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  bool sensorFailed = (sensorState == SENSOR_STATE_FAILED);
  bool sensorError = sensorLockout || sensorStale || sensorFailed;
  return sensorError;
}

// Appends one "\n  ..." line per sensor error under "errors:" in the status: the startup
// lockout, a stale sensor (with the time until it counts as failed) or a failed one (with what
// heat or cool does meanwhile).
void appendSensorErrors(uint64_t nowMs) {
  bool sensorLockout = (sensorState == SENSOR_STATE_LOCKOUT);
  bool sensorStale = (sensorState == SENSOR_STATE_STALE);
  bool sensorFailed = (sensorState == SENSOR_STATE_FAILED);
  const char *resultName = dhtResultName(sensorLastResult);
  unsigned long secondsWithoutReading = msToSeconds(msSince(sensorLastValidMs, nowMs));
  if (sensorLockout) {
    appendBody("\n  DHT22 not readable at startup (%s): HVAC locked out, every relay open", resultName);
  }
  if (sensorStale) {
    uint64_t failsInMs = msRemaining(sensorLastValidMs, DHT_FAILED_MS, nowMs);
    unsigned long failsInSeconds = msToSecondsRoundedUp(failsInMs);
    appendBody("\n  DHT22 no reading for %lus (%s): heat/cool paused, fails in %lus",
               secondsWithoutReading, resultName, failsInSeconds);
  }
  if (sensorFailed) {
    appendBody("\n  DHT22 failed, no reading for %lus (%s)", secondsWithoutReading, resultName);
    bool heatSelected = (settings.hvacMode == HVAC_MODE_HEAT);
    if (heatSelected) {
      appendBody("\n  heat running %u%% fallback until the sensor reads again", HEAT_FALLBACK_PERCENT);
    } else {
      appendBody("\n  cool not allowed until the sensor reads again");
    }
  }
}

// Returns true while a run limit has something to report: the alarm, a rest or a frozen reading.
bool runLimitsHaveError() {
  bool anyError = noResponseAlarm || runLimitResting || frozenReadingActive;
  return anyError;
}

// Appends the run limits in force as "\n  ..." lines under "errors:" in the status.
void appendRunLimitErrors(uint64_t nowMs) {
  if (noResponseAlarm) {
    appendBody("\n  %s: no temperature response in its last run", runLimitStageName);
  }
  if (runLimitResting) {
    uint64_t restLeftMs = msRemaining(runLimitRestStartMs, RUN_LIMIT_REST_MS, nowMs);
    unsigned long restSecondsLeft = msToSecondsRoundedUp(restLeftMs);
    appendBody("\n  %s stopped at a run limit: resting %lus more", runLimitStageName, restSecondsLeft);
  }
  if (frozenReadingActive) {
    appendBody("\n  reading frozen: heat running %u%% until it moves or the hvac mode is set", HEAT_FALLBACK_PERCENT);
  }
}

// Returns true once the interlocks have overridden a relay request since boot.
bool interlocksHaveOverridden() {
  bool overridden = (interlockRefusals > 0);
  return overridden;
}

// Appends the interlock override count as a "\n  ..." line under "errors:" in the status.
void appendInterlockErrors() {
    unsigned long refusalCount = interlockRefusals;
    appendBody("\n  %lu relay requests overridden by the interlocks (see log)", refusalCount);
}
