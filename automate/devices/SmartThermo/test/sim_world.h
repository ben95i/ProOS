// =====================================================================================
// test/sim_world.h
// SmartThermo host test: the simulated world the firmware runs in. One World lives in shared
// memory for a whole scenario; each boot of the firmware runs in a forked child process, so a
// restart or power loss starts the firmware from its pristine globals while the house, the
// clock, the flash (EEPROM) and the watchdog scratch registers carry on, as on the real Pico.
//
// Files:
//   sim_hal.cpp       clock, GPIO, DHT22 data line, EEPROM, Wi-Fi, watchdog, mutex, HTTP replies,
//                     trace output
//   sim_house.cpp     weather, house thermal model, humidity, DHT22 frames
//   sim_checks.cpp    the safety rules from the HVAC reference, checked on every relay change and
//                     every pass, plus run statistics
//   sim_scenarios.cpp the scenarios (normal operation, every API selection, every error)
//   thermo_units.cpp  unit tests of thermo.cpp's internal functions (includes thermo.cpp)
//   thermo_test.cpp   command line, scenario runner, boot and step loops
// =====================================================================================
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>

#include <hardware/watchdog.h>

// ---- firmware entry points (SmartThermo.ino) ----
void setup();
void loop();
void setup1();
void loop1();

const double SIM_PI = 3.14159265358979323846;

// ---- time ----
const uint64_t US_PER_MS_SIM = 1000;
const uint64_t US_PER_SECOND_SIM = 1000000;
const uint64_t US_PER_MINUTE_SIM = 60 * US_PER_SECOND_SIM;
const uint64_t US_PER_HOUR_SIM = 60 * US_PER_MINUTE_SIM;
const uint64_t US_PER_DAY_SIM = 24 * US_PER_HOUR_SIM;

// ---- pins (the same numbers as thermo.h; checked against it in thermo_units.cpp) ----
const uint8_t SIM_PIN_G = 16;
const uint8_t SIM_PIN_Y1 = 17;
const uint8_t SIM_PIN_W1 = 18;
const uint8_t SIM_PIN_DHT = 22;
const uint8_t SIM_PIN_COUNT = 72; // covers LED_BUILTIN (64)

// ---- the HVAC Thermostat Control Reference, as the checks apply it (seconds) ----
// Kept separate from thermo.cpp's constants on purpose: the checks test the firmware against
// the reference, not against itself.
const uint64_t SPEC_POWER_UP_DELAY_US = 300 * US_PER_SECOND_SIM;
const uint64_t SPEC_COMPRESSOR_MIN_OFF_US = 300 * US_PER_SECOND_SIM;
const uint64_t SPEC_COMPRESSOR_MIN_ON_US = 300 * US_PER_SECOND_SIM;
const uint64_t SPEC_FURNACE_MIN_ON_US = 180 * US_PER_SECOND_SIM;
const uint64_t SPEC_DEAD_BAND_US = 300 * US_PER_SECOND_SIM;
const uint64_t SPEC_FAN_RUN_ON_US = 60 * US_PER_SECOND_SIM;
const uint64_t SPEC_START_WINDOW_US = US_PER_HOUR_SIM;
const uint32_t SPEC_HEAT_STARTS_PER_HOUR = 6;
const uint32_t SPEC_COOL_STARTS_PER_HOUR = 4;
const uint64_t SPEC_EEPROM_MIN_INTERVAL_US = 300 * US_PER_SECOND_SIM;
const uint64_t SPEC_SENSOR_STALE_US = 60 * US_PER_SECOND_SIM;
const uint64_t SPEC_SENSOR_FAILED_US = 600 * US_PER_SECOND_SIM;
const uint64_t SPEC_SENSOR_STARTUP_US = 60 * US_PER_SECOND_SIM;
const uint64_t SPEC_TIMING_TOLERANCE_US = 1000; // the firmware counts whole milliseconds
const uint64_t SPEC_MAX_INTERRUPTS_OFF_US = 8000; // the DHT22 read holds interrupts off ~5 ms

// ---- settings values (common.h) and sensor states (thermo.h), as the API reports them ----
const int SIM_MODE_OFF = 0;
const int SIM_MODE_HEAT = 1;
const int SIM_MODE_COOL = 2;
const int SIM_FAN_AUTO = 0;
const int SIM_FAN_ON = 1;
const int SIM_SENSOR_OK = 0;
const int SIM_SENSOR_STARTING = 1;
const int SIM_SENSOR_LOCKOUT = 2;
const int SIM_SENSOR_STALE = 3;
const int SIM_SENSOR_FAILED = 4;

enum Season { SEASON_WINTER, SEASON_SPRING, SEASON_SUMMER, SEASON_FALL, SEASON_COUNT };
enum FanPlan { FAN_PLAN_SCHEDULE, FAN_PLAN_AUTO, FAN_PLAN_ON };

// what the DHT22's data line does on the next reads
enum SensorFault {
  SENSOR_FAULT_NONE,         // a good frame
  SENSOR_FAULT_UNPLUGGED,    // no answer: the pull-up holds the line high (firmware: no response)
  SENSOR_FAULT_DATA_TIMEOUT, // the sensor stops part way through its 40 bits (data timeout)
  SENSOR_FAULT_BAD_CHECKSUM, // a bit flipped by noise on a long cable (bad checksum)
  SENSOR_FAULT_OUT_OF_RANGE, // a dying sensor sends garbage with a valid checksum (out of range)
  SENSOR_FAULT_STUCK_LOW,    // the data line shorted to ground (no response)
  SENSOR_FAULT_COUNT
};

enum ResetKind {
  RESET_NONE,
  RESET_POWER_LOSS,       // power cut: everything but the flash is lost
  RESET_WATCHDOG_TIMEOUT, // the watchdog was not fed in time (a stall)
  RESET_WATCHDOG_REBOOT   // the firmware asked for a restart (the Wi-Fi restart)
};

// thrown inside a boot when the Pico resets; the runner starts the next boot
struct SimReset {
  ResetKind kind;
};

struct SimConfig {
  char scenarioName[48];
  char runLabel[64];         // scenario name plus season for season runs, used for file names
  Season season;
  double days;               // length of season runs
  uint32_t seed;
  uint32_t stepMs;           // simulated time between passes of the firmware loops
  FanPlan fanPlan;
  int setpointOverrideF;     // 0 = follow the daily schedule
  uint32_t sensorLogEvery;   // trace every Nth DHT22 reading
  uint32_t snapshotMinutes;  // minutes between WORLD lines in the trace
  bool randomFaults;         // season runs: inject random realistic faults every day
  double startHour;          // time of day the scenario starts at
};

struct WeatherOverride {
  bool active;
  double meanF;
  double swingF;
};

struct HouseState {
  double airF;          // indoor air temperature
  double massF;         // walls, floors and furniture
  double sensorF;       // the DHT22's own temperature, lagging the air
  double dewPointF;     // indoor dew point
  bool furnaceLockedOut; // the furnace ignores W1, as after an ignition lockout
  double furnaceOutput; // 0..1: the heat exchanger warms up after W1 closes, and cools after
  double coolingOutput; // 0..1: the coil cools down after Y1 closes, and warms after
  double outdoorF;
  double outdoorDewPointF;
  double relativeHumidity; // indoor, at the air temperature
  uint64_t updatedUs;      // world time the house was last advanced to
};

struct DhtSim {
  SensorFault fault;              // fault applied to every read while set
  uint32_t intermittentPercent;   // chance of a one-off checksum or timeout fault per read
  double spikeF;                  // added to the next good reading only, then cleared
  bool readingFrozen;             // every good reading is frozenReadingF, whatever the room does
  double frozenReadingF;
  uint64_t reads;                 // reads attempted by the firmware
  uint64_t faultedReads;          // reads answered with a fault
  // the latest read, for the trace
  bool readPending;
  uint8_t frame[5];
  SensorFault frameFault;
  double frameRoomF;
  double frameSensorF;
  double frameHumidity;
  double frameDhtC;
};

struct WiFiSim {
  bool accessPointUp;
  bool joined;
  uint64_t connectedAtUs; // world time the joined connection gets its address
  uint32_t beginCalls;
};

struct PinSim {
  int mode;          // INPUT (0) after reset
  int level;         // LOW (0) after reset
  int driveStrength; // gpio_drive_strength, GPIO_DRIVE_STRENGTH_4MA after reset
};

const uint32_t START_RECORD_CAPACITY = 16;
const uint32_t FAILURE_TEXT_CAPACITY = 4;
const uint32_t FAILURE_TEXT_LENGTH = 200;

// the safety rules' state, kept across boots (relay history matters across a restart)
struct CheckState {
  uint64_t checksRun;
  uint64_t failures;
  char failureTexts[FAILURE_TEXT_CAPACITY][FAILURE_TEXT_LENGTH];
  bool w1;
  bool y1;
  bool g;
  uint64_t w1RiseUs;
  uint64_t w1FallUs;
  uint64_t y1RiseUs;
  uint64_t y1FallUs;
  bool coolStopNormal;       // the last Y1 stop was a normal one, so G must run on
  uint64_t heatStartsUs[START_RECORD_CAPACITY];
  uint32_t heatStartCount;   // starts recorded, the newest at (count - 1) % capacity
  uint64_t coolStartsUs[START_RECORD_CAPACITY];
  uint32_t coolStartCount;
  uint32_t maxHeatStartsPerHour;
  uint32_t maxCoolStartsPerHour;
  uint32_t runOnsObserved;   // G held 60-90 s after a normal cool stop
  uint64_t lastCommitUptimeUs;
  bool commitSeenThisBoot;
  uint64_t maxInterruptsOffUs;
  bool checksEnabled;        // off for the unit tests, which drive the relays by hand
};

// statistics for one day (or the whole run)
struct RunStats {
  double airMinF;
  double airMaxF;
  double airSumF;
  double outdoorMinF;
  double outdoorMaxF;
  double humidityMin;
  double humidityMax;
  uint64_t samples;
  uint64_t w1Us;
  uint64_t y1Us;
  uint64_t gUs;
  uint32_t heatStarts;
  uint32_t coolStarts;
  uint64_t comfortCheckedUs; // time the comfort rule applied (heat or cool, settled, sensor ok)
  uint64_t comfortMissedUs;  // of that, time too cold in heat or too hot in cool
  uint64_t sensorReads;
  uint64_t sensorFaultedReads;
  uint32_t firmwareLogLines;
  uint32_t apiRequests;
  uint32_t eepromCommits;
  uint32_t boots;
};

// one firmware log line, kept so scenarios can look for it after a restart
const uint32_t FIRMWARE_LINE_CAPACITY = 1024;
const uint32_t FIRMWARE_LINE_LENGTH = 80;
struct FirmwareLine {
  uint64_t worldUs;
  char text[FIRMWARE_LINE_LENGTH];
};

struct World {
  SimConfig config;
  uint64_t nowUs;      // world clock, microseconds since the scenario started
  uint64_t bootUs;     // world time the current boot started (uptime = nowUs - bootUs)
  uint64_t stepFromUs; // the scenario events window (stepFromUs, stepToUs]
  uint64_t stepToUs;
  uint64_t endUs;      // the scenario ends here
  bool firmwareUp;     // false while the Pico is off (only the house and the clock run)
  uint32_t bootCount;
  ResetKind resetKind; // why the current boot ended
  uint64_t powerOffUs; // how long the power stays off after a RESET_POWER_LOSS
  uint64_t lastResetWorldUs;
  ResetKind lastResetKind;
  bool watchdogCausedReboot; // what watchdog_enable_caused_reboot() returns this boot
  watchdog_hw_t watchdogRegisters;
  bool watchdogEnabled;
  uint32_t watchdogTimeoutMs;
  uint64_t watchdogLastFeedUs; // uptime of the last feed
  uint64_t core0StallUntilUs;  // world time a simulated hang of core 0 ends (0 = none)
  uint64_t core1StallUntilUs;
  uint8_t flash[256];          // the EEPROM's flash sector
  uint32_t failCommitsRemaining;
  PinSim pins[SIM_PIN_COUNT];
  WeatherOverride weather;
  HouseState house;
  DhtSim dht;
  WiFiSim wifi;
  CheckState checks;
  RunStats day;
  RunStats total;
  uint32_t dayNumber;          // day of the stats in `day`, from 1
  int lastSeenMode;            // settings seen on the previous pass, to time the settling
  int lastSeenSetpointF;
  uint64_t settingsChangedUs;
  int lastSeenSensorState;
  uint64_t sensorProblemUs;    // world time the sensor was last not ok
  uint32_t powerLosses;
  uint32_t watchdogTimeouts;
  uint32_t wifiRestarts;
  uint64_t lastWatchdogResetUs;
  uint32_t scenarioChecks;
  uint32_t scenarioFailures;
  double scenarioValues[8];    // per-scenario accumulators (e.g. W1 time during a fallback)
  FirmwareLine firmwareLines[FIRMWARE_LINE_CAPACITY];
  uint32_t firmwareLineCount;  // lines captured, the newest at (count - 1) % capacity
};

extern World *world;
extern FILE *traceFile;
extern FILE *csvFile;

// ---- sim_hal.cpp ----
uint64_t simUptimeUs();
uint64_t simUptimeMs();
void simBeginBoot();                      // child-local state for a new boot
void simSetCurrentCore(int core);         // which core the next firmware calls come from
bool simCoreMayRun(int core);             // false while that core is busy, stalled or hung
bool simRelayClosed(uint8_t pin);         // the relay contact: output driven HIGH
int simPinDriveStrength(uint8_t pin);     // gpio_drive_strength set on the pin
void simResetPins();                      // what a reset does to the GPIO: inputs, relays open
bool simWiFiConnected();
void simSetAccessPoint(bool up);
std::string simRunCommand(uint16_t command, uint16_t value); // the API route, as http.cpp replies
void simForceNextDhtFrame(const uint8_t frame[5], SensorFault fault); // unit tests
bool simInterruptsEnabled();
int simMutexDepth();
uint32_t simRecentRelayWrites(uint8_t *pins, int *levels, uint32_t capacity);
void simClearRelayWrites();
void tracef(const char *category, const char *format, ...) __attribute__((format(printf, 2, 3)));
void formatWorldTime(char *output, size_t outputSize, uint64_t worldUs);
double simRandom(uint64_t first, uint64_t second, uint64_t third); // 0..1, repeatable per seed
double simGaussian(uint64_t first, uint64_t second, uint64_t third);

// ---- sim_house.cpp ----
const char *seasonName(Season season);
void houseInit(Season season, double indoorF);
void houseAdvanceTo(uint64_t worldUs);   // runs the physics with the relays as they are
double timeOfDayHours(uint64_t worldUs);
uint32_t worldDay(uint64_t worldUs);     // from 1
void buildDhtFrame(uint8_t frame[5], SensorFault &faultApplied);

// ---- sim_checks.cpp ----
void checkFail(const char *rule, const char *format, ...) __attribute__((format(printf, 2, 3)));
void checkRelayEdge(uint8_t pin, bool closed);  // every relay contact change, as it happens
void checkResetEdges();                         // relays opened by a reset
void checkAfterCore1Pass();                     // the per-pass rules
void checkEepromCommit();
void checkInterruptsOff(uint64_t offUs);
void statsAdvance(uint64_t fromUs, uint64_t toUs);
void statsFinishDay(bool finalDay);
void writeSnapshot();
void writeCsvRow();

// ---- sim_scenarios.cpp ----
struct ScenarioDef {
  const char *name;
  const char *description;
  Season season;          // weather (season runs use --season)
  double hours;           // length (season runs use --days)
  double startHour;       // time of day it starts
  double indoorF;         // indoor temperature at the start
  bool seasonRun;         // length and season from the command line
  bool assertComfort;     // fail when the room is too cold in heat or too hot in cool
  void (*prepare)();      // sets up the world before the first boot (flash, faults, weather)
  void (*events)();       // called once per step window, also while the Pico is off
  void (*finish)();       // final checks, with the firmware running
};
const ScenarioDef *scenarioTable(size_t &count);
void scenarioExpect(bool passed, const char *format, ...) __attribute__((format(printf, 2, 3)));

// ---- thermo_test.cpp ----
void drainFirmwareLog();                                      // traces new firmware log lines
bool firmwareLogContains(double fromMinute, const char *text); // logged at or after that minute
void traceMultiline(const char *category, const std::string &text);

// ---- thermo_units.cpp ----
int runUnitTests(); // returns the number of failed unit tests
