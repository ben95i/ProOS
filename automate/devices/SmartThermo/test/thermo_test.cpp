// =====================================================================================
// test/thermo_test.cpp
// SmartThermo host test: the command line and the runner.
//
// Every scenario gets a fresh World in shared memory. Each boot of the firmware runs in a forked
// child: it calls setup() and setup1(), then steps both cores' loops through simulated time
// until the scenario ends or the Pico resets (power cut, watchdog, the firmware's own restart).
// The parent then keeps the house and clock running while the Pico is off and forks the next
// boot, which starts from the firmware's pristine globals, as a real reset does.
//
// Output, per run, in the output folder: <run>.log (the detailed trace) and <run>.csv (one row
// per simulated minute), plus summary.txt. See test/README.md.
// =====================================================================================
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../thermo.h"
#include "sim_world.h"

// SmartThermo.ino's rolling log, read after every pass to copy new lines into the trace
const uint8_t FIRMWARE_LOG_CAPACITY = 60;
struct LogEntry {
  uint64_t uptimeMs;
  char text[80];
};
extern LogEntry logBuffer[];
extern uint8_t logNextIndex;

const char *sensorStateName(int16_t state); // thermo.cpp

const int BOOT_FINISHED = 0;
const int BOOT_RESET = 10;
const uint64_t REBOOT_US = 200000; // a watchdog reset and the boot ROM take about this long
const uint32_t STEP_MS_MIN = 10;
const uint32_t STEP_MS_MAX = 1000;
const uint64_t CSV_INTERVAL_US = US_PER_MINUTE_SIM;

static uint8_t firmwareLogSeen = 0; // child-local: the next rolling-log entry to copy

// ==== firmware log ====

// Copies the firmware's new log lines into the trace and the World's line store.
void drainFirmwareLog() {
  if (!world->firmwareUp) {
    return;
  }
  while (firmwareLogSeen != logNextIndex) {
    const LogEntry &entry = logBuffer[firmwareLogSeen];
    tracef("FW", "%s", entry.text);
    uint32_t slot = world->firmwareLineCount % FIRMWARE_LINE_CAPACITY;
    world->firmwareLines[slot].worldUs = world->nowUs;
    snprintf(world->firmwareLines[slot].text, FIRMWARE_LINE_LENGTH, "%s", entry.text);
    world->firmwareLineCount = world->firmwareLineCount + 1;
    world->day.firmwareLogLines = world->day.firmwareLogLines + 1;
    world->total.firmwareLogLines = world->total.firmwareLogLines + 1;
    firmwareLogSeen = (uint8_t)((firmwareLogSeen + 1) % FIRMWARE_LOG_CAPACITY);
  }
}

// True when the firmware logged a line holding the text at or after the given minute.
bool firmwareLogContains(double fromMinute, const char *text) {
  uint64_t fromUs = (uint64_t)llround(fromMinute * (double)US_PER_MINUTE_SIM);
  uint32_t stored = world->firmwareLineCount;
  if (stored > FIRMWARE_LINE_CAPACITY) {
    stored = FIRMWARE_LINE_CAPACITY;
  }
  for (uint32_t age = 0; age < stored; age++) {
    uint32_t slot = (world->firmwareLineCount - 1 - age) % FIRMWARE_LINE_CAPACITY;
    const FirmwareLine &line = world->firmwareLines[slot];
    if (line.worldUs < fromUs) {
      return false;
    }
    if (strstr(line.text, text) != nullptr) {
      return true;
    }
  }
  return false;
}

// Traces a multi-line reply one line at a time, indented under its request.
void traceMultiline(const char *category, const std::string &text) {
  size_t lineStart = 0;
  while (lineStart <= text.size()) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = text.size();
    }
    std::string line = text.substr(lineStart, lineEnd - lineStart);
    tracef(category, "  | %s", line.c_str());
    lineStart = lineEnd + 1;
  }
}

// ==== sensor trace ====

static const char *frameFaultText(SensorFault fault) {
  switch (fault) {
    case SENSOR_FAULT_UNPLUGGED:
      return "FAULT unplugged: no answer";
    case SENSOR_FAULT_DATA_TIMEOUT:
      return "FAULT stopped part way through the frame";
    case SENSOR_FAULT_BAD_CHECKSUM:
      return "FAULT one bit flipped on the line";
    case SENSOR_FAULT_OUT_OF_RANGE:
      return "FAULT garbage frame";
    case SENSOR_FAULT_STUCK_LOW:
      return "FAULT data line stuck low";
    default:
      return "";
  }
}

// Traces the DHT22 reading the firmware just took: the room, what the sensor sent and what the
// firmware made of it, e.g.
// "room air 69.41 F, at sensor 69.38 F, RH 31.2 % | DHT22 sent 01 38 00 D0 09 (20.8 C, 31.2 %) | firmware: 69.4 F, 31.2 %, sensor ok"
static void traceSensorRead() {
  DhtSim &dht = world->dht;
  if (!dht.readPending) {
    return;
  }
  dht.readPending = false;
  world->day.sensorReads = world->day.sensorReads + 1;
  world->total.sensorReads = world->total.sensorReads + 1;
  bool faulted = (dht.frameFault != SENSOR_FAULT_NONE);
  if (faulted) {
    world->day.sensorFaultedReads = world->day.sensorFaultedReads + 1;
    world->total.sensorFaultedReads = world->total.sensorFaultedReads + 1;
  }
  uint32_t every = world->config.sensorLogEvery;
  bool regularLine = (every > 0) && (dht.reads % every == 0);
  if (!faulted && !regularLine) {
    return;
  }
  char temperatureText[TEMPERATURE_TEXT_SIZE];
  char humidityText[TEMPERATURE_TEXT_SIZE];
  formatTemperatureReading(temperatureText, sizeof(temperatureText));
  formatHumidityReading(humidityText, sizeof(humidityText));
  const char *stateName = sensorStateName(currentSensorState());
  bool lineCarriedFrame = (dht.frameFault != SENSOR_FAULT_UNPLUGGED) && (dht.frameFault != SENSOR_FAULT_STUCK_LOW);
  char sentText[96];
  if (lineCarriedFrame) {
    snprintf(sentText, sizeof(sentText), "DHT22 sent %02X %02X %02X %02X %02X (%.1f C, %.1f %%)%s%s", dht.frame[0], dht.frame[1],
             dht.frame[2], dht.frame[3], dht.frame[4], dht.frameDhtC, dht.frameHumidity, faulted ? " " : "",
             frameFaultText(dht.frameFault));
  } else {
    snprintf(sentText, sizeof(sentText), "DHT22 %s", frameFaultText(dht.frameFault));
  }
  tracef("SENSOR", "room air %.2f F, at sensor %.2f F, RH %.1f %% | %s | firmware: %s F, %s %%, sensor %s", dht.frameRoomF,
         dht.frameSensorF, world->house.relativeHumidity, sentText, temperatureText, humidityText, stateName);
}

// ==== stepping ====

static uint64_t stepUs() {
  return (uint64_t)world->config.stepMs * US_PER_MS_SIM;
}

static bool crossed(uint64_t fromUs, uint64_t toUs, uint64_t intervalUs) {
  if (intervalUs == 0) {
    return false;
  }
  bool crossedBoundary = (fromUs / intervalUs) != (toUs / intervalUs);
  return crossedBoundary;
}

// Moves the world from fromUs to toUs: house physics, statistics, the CSV and WORLD lines, the
// day summaries, then the scenario's events for the window (fromUs, toUs]. Used both while the
// firmware runs and while the Pico is off.
static void advanceWorld(const ScenarioDef &scenario, uint64_t fromUs, uint64_t toUs) {
  if (world->nowUs < toUs) {
    world->nowUs = toUs;
  }
  houseAdvanceTo(toUs);
  statsAdvance(fromUs, toUs);
  if (crossed(fromUs, toUs, CSV_INTERVAL_US)) {
    writeCsvRow();
  }
  uint64_t snapshotUs = (uint64_t)world->config.snapshotMinutes * US_PER_MINUTE_SIM;
  if (crossed(fromUs, toUs, snapshotUs)) {
    writeSnapshot();
  }
  uint32_t dayNow = worldDay(toUs);
  if (dayNow != world->dayNumber) {
    if (world->firmwareUp) {
      std::string status = simRunCommand(10, 0);
      tracef("STATUS", "end of day %u status (?10,0,0~):", world->dayNumber);
      traceMultiline("STATUS", status);
      drainFirmwareLog();
    }
    statsFinishDay(false);
  }
  world->stepFromUs = fromUs;
  world->stepToUs = toUs;
  simSetCurrentCore(0);
  scenario.events();
  drainFirmwareLog();
}

// Resets the Pico when the watchdog has not been fed within its timeout.
static void checkWatchdog() {
  if (!world->watchdogEnabled) {
    return;
  }
  uint64_t sinceFeedUs = simUptimeUs() - world->watchdogLastFeedUs;
  uint64_t timeoutUs = (uint64_t)world->watchdogTimeoutMs * US_PER_MS_SIM;
  if (sinceFeedUs <= timeoutUs) {
    return;
  }
  tracef("WDOG", "watchdog not fed for %.1f s: it resets the Pico", (double)sinceFeedUs / (double)US_PER_SECOND_SIM);
  world->lastWatchdogResetUs = world->nowUs;
  SimReset reset;
  reset.kind = RESET_WATCHDOG_TIMEOUT;
  throw reset;
}

// One step: core 1's pass (sensor and thermostat) and core 0's pass (Wi-Fi, API, EEPROM) at the
// step time, unless the core is busy or hung, then the world moves on by one step.
static void runStep(const ScenarioDef &scenario) {
  uint64_t passUs = world->stepToUs;
  if (simCoreMayRun(1)) {
    simSetCurrentCore(1);
    loop1();
    drainFirmwareLog();
    traceSensorRead();
    checkAfterCore1Pass();
  }
  if (simCoreMayRun(0)) {
    simSetCurrentCore(0);
    loop();
    drainFirmwareLog();
  }
  checkWatchdog();
  uint64_t nextUs = passUs + stepUs();
  advanceWorld(scenario, passUs, nextUs);
}

static const char *bootCauseText() {
  if (world->bootCount == 1) {
    return "first power-up, flash as prepared";
  }
  switch (world->lastResetKind) {
    case RESET_POWER_LOSS:
      return "power restored";
    case RESET_WATCHDOG_TIMEOUT:
      return "after a watchdog reset";
    case RESET_WATCHDOG_REBOOT:
      return "after the firmware's own restart";
    default:
      return "?";
  }
}

// One boot of the firmware, in a forked child. Returns BOOT_FINISHED at the scenario's end or
// BOOT_RESET when the Pico reset (world->resetKind says why).
static int runBoot(const ScenarioDef &scenario) {
  world->bootCount = world->bootCount + 1;
  world->firmwareUp = true;
  world->day.boots = world->day.boots + 1;
  world->total.boots = world->total.boots + 1;
  world->stepFromUs = world->nowUs;
  world->stepToUs = world->nowUs;
  simBeginBoot();
  firmwareLogSeen = 0;
  tracef("BOOT", "boot %u (%s)", world->bootCount, bootCauseText());
  try {
    simSetCurrentCore(0);
    setup();
    drainFirmwareLog();
    simSetCurrentCore(1);
    setup1();
    while (world->nowUs < world->endUs) {
      runStep(scenario);
    }
    if (scenario.finish != nullptr) {
      simSetCurrentCore(0);
      scenario.finish();
    }
    std::string status = simRunCommand(10, 0);
    tracef("STATUS", "final status (?10,0,0~):");
    traceMultiline("STATUS", status);
    drainFirmwareLog();
    return BOOT_FINISHED;
  } catch (const SimReset &reset) {
    world->resetKind = reset.kind;
    drainFirmwareLog();
    return BOOT_RESET;
  }
}

static const char *resetKindText(ResetKind kind) {
  switch (kind) {
    case RESET_POWER_LOSS:
      return "POWER LOST: the Pico stops, every relay opens";
    case RESET_WATCHDOG_TIMEOUT:
      return "WATCHDOG RESET: every pin resets, every relay opens";
    case RESET_WATCHDOG_REBOOT:
      return "RESTART requested by the firmware: every pin resets, every relay opens";
    default:
      return "reset";
  }
}

// In the parent after a boot ended in a reset: opens the relays, keeps the world running while
// the Pico is off (or rebooting) and sets the clock for the next boot.
static void handleReset(const ScenarioDef &scenario) {
  ResetKind kind = world->resetKind;
  tracef("RESET", "%s", resetKindText(kind));
  checkResetEdges();
  simResetPins();
  world->firmwareUp = false;
  world->watchdogEnabled = false;
  world->wifi.joined = false;
  world->core0StallUntilUs = 0;
  world->core1StallUntilUs = 0;
  world->lastResetKind = kind;
  world->lastResetWorldUs = world->nowUs;
  uint64_t offUs = REBOOT_US;
  if (kind == RESET_POWER_LOSS) {
    world->powerLosses = world->powerLosses + 1;
    world->watchdogCausedReboot = false;
    memset(&world->watchdogRegisters, 0, sizeof(world->watchdogRegisters)); // power loss clears the scratch registers
    offUs = world->powerOffUs;
  } else if (kind == RESET_WATCHDOG_TIMEOUT) {
    world->watchdogTimeouts = world->watchdogTimeouts + 1;
    world->watchdogCausedReboot = true;
  } else {
    world->wifiRestarts = world->wifiRestarts + 1;
    world->watchdogCausedReboot = false;
  }
  uint64_t bootAtUs = world->nowUs + offUs;
  uint64_t fromUs = world->stepToUs;
  while (fromUs < bootAtUs && fromUs < world->endUs) {
    uint64_t toUs = fromUs + stepUs();
    if (toUs > bootAtUs) {
      toUs = bootAtUs;
    }
    advanceWorld(scenario, fromUs, toUs);
    fromUs = toUs;
  }
  if (world->nowUs < bootAtUs) {
    world->nowUs = bootAtUs;
  }
  world->bootUs = world->nowUs;
}

// ==== runs ====

struct RunResult {
  std::string label;
  bool passed;
  bool finished;
  uint64_t checksRun;
  uint64_t failures;
  uint32_t scenarioChecks;
  uint32_t scenarioFailures;
  uint32_t boots;
  double hours;
  double heatPercent;
  double coolPercent;
  double comfortPercent;
  bool comfortApplies;
  std::vector<std::string> failureTexts;
};

static std::string outputFolder = "output";

static FILE *openOutput(const std::string &label, const char *extension) {
  std::string path = outputFolder + "/" + label + extension;
  FILE *file = fopen(path.c_str(), "w");
  if (file == nullptr) {
    fprintf(stderr, "cannot write %s: %s\n", path.c_str(), strerror(errno));
    exit(2);
  }
  return file;
}

static void prepareWorld(const SimConfig &config, const std::string &label) {
  memset(world, 0, sizeof(World));
  world->config = config;
  snprintf(world->config.runLabel, sizeof(world->config.runLabel), "%s", label.c_str());
  memset(world->flash, 0xFF, sizeof(world->flash)); // a freshly flashed Pico: erased EEPROM sector
  world->wifi.accessPointUp = true;
  simResetPins();
  world->dayNumber = worldDay(0);
  world->lastSeenMode = -1;
  world->lastSeenSetpointF = -1;
  world->lastSeenSensorState = -1;
  world->checks.checksEnabled = true;
  watchdog_hw = &world->watchdogRegisters;
}

static double percentOf(uint64_t part, uint64_t whole) {
  if (whole == 0) {
    return 0.0;
  }
  return 100.0 * (double)part / (double)whole;
}

static RunResult collectResult(const std::string &label, bool finished, double hours, bool assertComfort) {
  RunResult result;
  result.label = label;
  result.finished = finished;
  result.checksRun = world->checks.checksRun;
  result.failures = world->checks.failures;
  result.scenarioChecks = world->scenarioChecks;
  result.scenarioFailures = world->scenarioFailures;
  result.boots = world->bootCount;
  result.hours = hours;
  uint64_t sampledUs = world->total.samples * stepUs();
  result.heatPercent = percentOf(world->total.w1Us, sampledUs);
  result.coolPercent = percentOf(world->total.y1Us, sampledUs);
  result.comfortApplies = world->total.comfortCheckedUs > 0;
  result.comfortPercent = 100.0 - percentOf(world->total.comfortMissedUs, world->total.comfortCheckedUs);
  uint64_t storedFailures = world->checks.failures;
  if (storedFailures > FAILURE_TEXT_CAPACITY) {
    storedFailures = FAILURE_TEXT_CAPACITY;
  }
  for (uint64_t index = 0; index < storedFailures; index++) {
    result.failureTexts.push_back(world->checks.failureTexts[index]);
  }
  if (assertComfort && result.comfortApplies && result.comfortPercent < 98.0) {
    char text[FAILURE_TEXT_LENGTH];
    snprintf(text, sizeof(text), "[comfort] the room was too cold in heat or too hot in cool with the equipment idle for %.1f %% of settled time",
             100.0 - result.comfortPercent);
    result.failureTexts.push_back(text);
    result.failures = result.failures + 1;
  }
  result.passed = (result.failures == 0);
  return result;
}

static void writeRunHeader(const ScenarioDef &scenario, double hours) {
  const SimConfig &config = world->config;
  tracef("RUN", "SmartThermo host test: %s", config.runLabel);
  tracef("RUN", "%s", scenario.description);
  tracef("RUN", "season %s, %.2f h from %02d:%02d, indoor %.1f F at the start, seed %u, step %u ms", seasonName(config.season), hours,
         (int)config.startHour, (int)((config.startHour - floor(config.startHour)) * 60.0), scenario.indoorF, config.seed,
         config.stepMs);
  tracef("RUN", "trace: SENSOR every %u DHT22 read(s) and every faulted one, GPIO every relay pin write, FW every firmware "
         "log line, API every request, WORLD every %u min, DAY summaries", config.sensorLogEvery, config.snapshotMinutes);
}

// Runs one scenario (one season for season runs) from a pristine firmware to its end.
static RunResult runScenario(const ScenarioDef &scenario, const SimConfig &baseConfig, Season season) {
  SimConfig config = baseConfig;
  config.season = season;
  config.startHour = scenario.startHour;
  config.randomFaults = (strcmp(scenario.name, "season-faults") == 0);
  snprintf(config.scenarioName, sizeof(config.scenarioName), "%s", scenario.name);
  std::string label = scenario.name;
  if (scenario.seasonRun) {
    label = label + "-" + seasonName(season);
  }
  prepareWorld(config, label);
  double hours = scenario.seasonRun ? baseConfig.days * 24.0 : scenario.hours;
  world->endUs = (uint64_t)llround(hours * (double)US_PER_HOUR_SIM);
  traceFile = openOutput(label, ".log");
  csvFile = openOutput(label, ".csv");
  fprintf(csvFile, "minutes,time,outdoor_f,indoor_air_f,structure_f,at_sensor_f,rh_percent,setpoint_f,mode,fan,w1,y1,g,sensor_state,pico_on\n");
  houseInit(season, scenario.indoorF);
  writeRunHeader(scenario, hours);
  if (scenario.prepare != nullptr) {
    scenario.prepare();
  }
  bool finished = false;
  while (true) {
    fflush(traceFile);
    fflush(csvFile);
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
      checkFail("runner", "fork failed: %s", strerror(errno));
      break;
    }
    if (child == 0) {
      int bootResult = runBoot(scenario);
      fflush(traceFile);
      fflush(csvFile);
      _exit(bootResult);
    }
    int status = 0;
    waitpid(child, &status, 0);
    bool exited = WIFEXITED(status);
    int exitCode = exited ? WEXITSTATUS(status) : -1;
    if (exited && exitCode == BOOT_FINISHED) {
      finished = true;
      break;
    }
    if (exited && exitCode == BOOT_RESET) {
      handleReset(scenario);
      if (world->nowUs >= world->endUs) {
        tracef("RUN", "the run ended while the Pico was off: its final checks were skipped");
        break;
      }
      continue;
    }
    world->firmwareUp = false;
    if (WIFSIGNALED(status)) {
      checkFail("crash", "boot %u crashed with signal %d", world->bootCount, WTERMSIG(status));
    } else {
      checkFail("crash", "boot %u ended with exit code %d", world->bootCount, exitCode);
    }
    break;
  }
  world->firmwareUp = false;
  statsFinishDay(true);
  RunResult result = collectResult(label, finished, hours, scenario.assertComfort);
  tracef("RUN", "%s: %llu rule checks, %llu failures, %u scenario expectations, %u failed", result.passed ? "PASSED" : "FAILED",
         (unsigned long long)result.checksRun, (unsigned long long)result.failures, result.scenarioChecks, result.scenarioFailures);
  fclose(traceFile);
  fclose(csvFile);
  traceFile = nullptr;
  csvFile = nullptr;
  return result;
}

// Runs the unit tests in a forked child (they change the firmware's globals).
static RunResult runUnits(const SimConfig &baseConfig) {
  SimConfig config = baseConfig;
  config.season = SEASON_WINTER;
  config.startHour = 0.0;
  prepareWorld(config, "units");
  world->checks.checksEnabled = false;
  world->endUs = 0;
  traceFile = openOutput("units", ".log");
  csvFile = nullptr;
  houseInit(SEASON_WINTER, 70.0);
  tracef("RUN", "SmartThermo host test: unit tests of thermo.cpp");
  fflush(traceFile);
  fflush(stdout);
  pid_t child = fork();
  if (child == 0) {
    world->firmwareUp = true;
    world->bootCount = 1;
    simBeginBoot();
    firmwareLogSeen = 0;
    int failures = runUnitTests();
    fflush(traceFile);
    _exit(failures > 100 ? 100 : failures);
  }
  int status = 0;
  waitpid(child, &status, 0);
  world->firmwareUp = false;
  if (WIFSIGNALED(status)) {
    checkFail("crash", "unit tests crashed with signal %d", WTERMSIG(status));
  }
  RunResult result = collectResult("units", true, 0.0, false);
  result.checksRun = result.checksRun + world->scenarioChecks;
  tracef("RUN", "%s", result.passed ? "PASSED" : "FAILED");
  fclose(traceFile);
  traceFile = nullptr;
  return result;
}

// ==== command line ====

static void printUsage() {
  printf("SmartThermo host test: runs thermo.cpp and SmartThermo.ino on simulated hardware.\n\n"
         "usage: thermo_test [options]\n"
         "  --scenario NAMES   comma-separated: all (default), units, scenarios (every fixed scenario),\n"
         "                     seasons (season and season-faults), or scenario names (see --list)\n"
         "  --days N           length of season runs in days (default 3; 50+ also crosses the 49.7-day millis() wrap)\n"
         "  --season S         winter, spring, summer, fall or all (default all) for season runs\n"
         "  --fan PLAN         season runs: schedule (default; fan on 3 h every other day), auto or on\n"
         "  --setpoint F       season runs: hold this setpoint (50-80) instead of the daily schedule\n"
         "  --seed N           weather, sensor noise and random faults (default 1)\n"
         "  --step-ms N        simulated time between loop passes, 10-1000 (default 1000)\n"
         "  --sensor-log N     trace every Nth DHT22 reading (default 1 = every reading; faulted reads always)\n"
         "  --snapshot N       minutes between WORLD lines (default 15)\n"
         "  --out DIR          output folder (default ./output)\n"
         "  --list             list the scenarios\n");
}

static bool parseUnsigned(const char *text, long minimum, long maximum, long &value) {
  char *end = nullptr;
  long parsed = strtol(text, &end, 10);
  bool valid = (end != text) && (*end == '\0') && (parsed >= minimum) && (parsed <= maximum);
  if (valid) {
    value = parsed;
  }
  return valid;
}

static std::vector<std::string> splitList(const std::string &text) {
  std::vector<std::string> items;
  std::string item;
  for (char character : text) {
    if (character == ',') {
      if (!item.empty()) {
        items.push_back(item);
      }
      item.clear();
    } else {
      item.push_back(character);
    }
  }
  if (!item.empty()) {
    items.push_back(item);
  }
  return items;
}

struct PlannedRun {
  const ScenarioDef *scenario; // nullptr = the unit tests
  Season season;
};

int main(int argumentCount, char **arguments) {
  SimConfig config;
  memset(&config, 0, sizeof(config));
  config.days = 3.0;
  config.seed = 1;
  config.stepMs = 1000;
  config.fanPlan = FAN_PLAN_SCHEDULE;
  config.setpointOverrideF = 0;
  config.sensorLogEvery = 1;
  config.snapshotMinutes = 15;
  std::string scenarioList = "all";
  std::string seasonChoice = "all";
  size_t scenarioCount = 0;
  const ScenarioDef *scenarios = scenarioTable(scenarioCount);

  for (int index = 1; index < argumentCount; index++) {
    std::string option = arguments[index];
    bool hasValue = (index + 1 < argumentCount);
    const char *value = hasValue ? arguments[index + 1] : "";
    long number = 0;
    if (option == "--help" || option == "-h") {
      printUsage();
      return 0;
    }
    if (option == "--list") {
      printf("units                 unit tests of thermo.cpp's functions\n");
      for (size_t scenarioIndex = 0; scenarioIndex < scenarioCount; scenarioIndex++) {
        printf("%-21s %s\n", scenarios[scenarioIndex].name, scenarios[scenarioIndex].description);
      }
      return 0;
    }
    if (!hasValue) {
      fprintf(stderr, "%s needs a value (see --help)\n", option.c_str());
      return 2;
    }
    index = index + 1;
    if (option == "--scenario") {
      scenarioList = value;
    } else if (option == "--days") {
      char *end = nullptr;
      double days = strtod(value, &end);
      if (end == value || *end != '\0' || days <= 0.0 || days > 3650.0) {
        fprintf(stderr, "--days must be a number of days, e.g. 7 or 0.5\n");
        return 2;
      }
      config.days = days;
    } else if (option == "--season") {
      seasonChoice = value;
    } else if (option == "--fan") {
      std::string plan = value;
      if (plan == "schedule") {
        config.fanPlan = FAN_PLAN_SCHEDULE;
      } else if (plan == "auto") {
        config.fanPlan = FAN_PLAN_AUTO;
      } else if (plan == "on") {
        config.fanPlan = FAN_PLAN_ON;
      } else {
        fprintf(stderr, "--fan must be schedule, auto or on\n");
        return 2;
      }
    } else if (option == "--setpoint" && parseUnsigned(value, 50, 80, number)) {
      config.setpointOverrideF = (int)number;
    } else if (option == "--seed" && parseUnsigned(value, 0, 2147483647L, number)) {
      config.seed = (uint32_t)number;
    } else if (option == "--step-ms" && parseUnsigned(value, STEP_MS_MIN, STEP_MS_MAX, number)) {
      config.stepMs = (uint32_t)number;
    } else if (option == "--sensor-log" && parseUnsigned(value, 1, 1000000, number)) {
      config.sensorLogEvery = (uint32_t)number;
    } else if (option == "--snapshot" && parseUnsigned(value, 1, 1440, number)) {
      config.snapshotMinutes = (uint32_t)number;
    } else if (option == "--out") {
      outputFolder = value;
    } else {
      fprintf(stderr, "bad option or value: %s %s (see --help)\n", option.c_str(), value);
      return 2;
    }
  }

  std::vector<Season> seasons;
  if (seasonChoice == "all") {
    seasons = { SEASON_WINTER, SEASON_SPRING, SEASON_SUMMER, SEASON_FALL };
  } else {
    for (int season = 0; season < SEASON_COUNT; season++) {
      if (seasonChoice == seasonName((Season)season)) {
        seasons.push_back((Season)season);
      }
    }
    if (seasons.empty()) {
      fprintf(stderr, "--season must be winter, spring, summer, fall or all\n");
      return 2;
    }
  }

  std::vector<PlannedRun> plan;
  for (const std::string &name : splitList(scenarioList)) {
    bool everything = (name == "all");
    bool matched = false;
    if (everything || name == "units") {
      plan.push_back({ nullptr, SEASON_WINTER });
      matched = true;
    }
    for (size_t scenarioIndex = 0; scenarioIndex < scenarioCount; scenarioIndex++) {
      const ScenarioDef &scenario = scenarios[scenarioIndex];
      bool groupMatch = (everything) || (name == "scenarios" && !scenario.seasonRun) || (name == "seasons" && scenario.seasonRun);
      if (!groupMatch && name != scenario.name) {
        continue;
      }
      matched = true;
      if (!scenario.seasonRun) {
        plan.push_back({ &scenario, scenario.season });
        continue;
      }
      for (Season season : seasons) {
        plan.push_back({ &scenario, season });
      }
    }
    if (!matched) {
      fprintf(stderr, "unknown scenario \"%s\" (see --list)\n", name.c_str());
      return 2;
    }
  }

  if (mkdir(outputFolder.c_str(), 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "cannot create %s: %s\n", outputFolder.c_str(), strerror(errno));
    return 2;
  }
  void *shared = mmap(nullptr, sizeof(World), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED) {
    fprintf(stderr, "cannot map the shared world: %s\n", strerror(errno));
    return 2;
  }
  world = (World *)shared;

  printf("SmartThermo host test: %zu runs, output in %s/\n\n", plan.size(), outputFolder.c_str());
  std::vector<RunResult> results;
  for (const PlannedRun &run : plan) {
    std::string label = (run.scenario == nullptr) ? "units" : run.scenario->name;
    if (run.scenario != nullptr && run.scenario->seasonRun) {
      label = label + "-" + seasonName(run.season);
    }
    printf("  running %-26s", label.c_str());
    fflush(stdout);
    RunResult result = (run.scenario == nullptr) ? runUnits(config) : runScenario(*run.scenario, config, run.season);
    char comfortText[24] = "";
    if (result.comfortApplies) {
      snprintf(comfortText, sizeof(comfortText), "comfort %5.1f%%", result.comfortPercent);
    }
    printf("\r  %s %-26s %7.1f h  %u boot(s)  %9llu checks  %4u expectations  heat %4.1f%%  cool %4.1f%%  %s\n",
           result.passed ? "PASS" : "FAIL", result.label.c_str(), result.hours, result.boots, (unsigned long long)result.checksRun,
           result.scenarioChecks, result.heatPercent, result.coolPercent, comfortText);
    for (const std::string &failure : result.failureTexts) {
      printf("         %s\n", failure.c_str());
    }
    if (result.failures > result.failureTexts.size()) {
      printf("         ... %llu failures in all, see %s/%s.log (grep FAIL)\n", (unsigned long long)result.failures,
             outputFolder.c_str(), result.label.c_str());
    }
    fflush(stdout);
    results.push_back(result);
  }

  int failedRuns = 0;
  std::string summaryPath = outputFolder + "/summary.txt";
  FILE *summary = fopen(summaryPath.c_str(), "w");
  for (const RunResult &result : results) {
    if (!result.passed) {
      failedRuns = failedRuns + 1;
    }
    if (summary != nullptr) {
      fprintf(summary, "%s %s %.1f h, %u boots, %llu checks, %u expectations, %llu failures\n", result.passed ? "PASS" : "FAIL",
              result.label.c_str(), result.hours, result.boots, (unsigned long long)result.checksRun, result.scenarioChecks,
              (unsigned long long)result.failures);
      for (const std::string &failure : result.failureTexts) {
        fprintf(summary, "    %s\n", failure.c_str());
      }
    }
  }
  if (summary != nullptr) {
    fclose(summary);
  }
  printf("\n%s: %zu runs, %d failed. Traces and CSVs in %s/\n", failedRuns == 0 ? "ALL PASSED" : "FAILURES", results.size(),
         failedRuns, outputFolder.c_str());
  munmap(shared, sizeof(World));
  return failedRuns == 0 ? 0 : 1;
}
