// =====================================================================================
// test/sim_checks.cpp
// SmartThermo host test: the safety rules of the HVAC Thermostat Control Reference, checked on
// the GPIO pins themselves (what the equipment would see), plus the run statistics.
//
// On every relay contact change (checkRelayEdge):
//   - W1 and Y1 are never closed together
//   - Y1 closes only with G already closed, opens before G, and G runs on 60 s after a normal stop
//   - Y1 closes only 5 min after it last opened (also across restarts and power loss), 5 min after
//     W1 opened (dead band), 5 min after boot, in cool mode, on a fresh reading (never stale), at
//     most 4 times an hour, and runs at least 5 min unless switched off or the sensor failed
//   - W1 closes only 5 min after Y1 opened, 5 min after boot, in heat mode, on a fresh reading or
//     the failed-sensor fallback (never stale), at most 6 times an hour, and runs at least 3 min
//     unless switched off
//   - W1 runs at most 3 h and Y1 at most 6 h in one run
// After every core 1 pass (checkAfterCore1Pass):
//   - the mutex is free and interrupts are on; the pins match the firmware's relay state
//   - startup lockout: every relay open; failed sensor, also with no reading since boot in heat
//     mode: no cooling and cool mode switched off
//   - stale sensor: a running furnace or compressor stops once its minimum on time is over
//   - mode off: W1 and Y1 open; fan on: G closed; fan auto: G open on heat and when idle
//   - the final interlock never had to override the thermostat logic
// EEPROM: at most one write per 5 minutes, the first no sooner than 5 minutes after boot.
// =====================================================================================
#include <math.h>
#include <stdarg.h>

#include "../thermo.h"
#include "sim_world.h"

const uint64_t COMFORT_SETTLE_US = 90 * US_PER_MINUTE_SIM; // after a change, the room gets this long to settle
const double COMFORT_BAND_F = 2.0;                           // too cold in heat / too hot in cool beyond this
const uint64_t RUN_ON_OBSERVED_MAX_US = 90 * US_PER_SECOND_SIM;
const uint64_t PASS_TOLERANCE_US = 2000; // a pass's decisions use the time it started
const uint64_t STALE_STOP_TOLERANCE_US = 10000; // the pass that stops it may follow a DHT22 read (~5 ms)
const uint64_t SPEC_HEAT_MAX_RUN_US = 180 * US_PER_MINUTE_SIM;
const uint64_t SPEC_COOL_MAX_RUN_US = 360 * US_PER_MINUTE_SIM;

// Records a failed rule: counted, kept for the summary (the first few) and traced.
void checkFail(const char *rule, const char *format, ...) {
  char text[FAILURE_TEXT_LENGTH];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  CheckState &checks = world->checks;
  if (checks.failures < FAILURE_TEXT_CAPACITY) {
    char whenText[32];
    formatWorldTime(whenText, sizeof(whenText), world->nowUs);
    snprintf(checks.failureTexts[checks.failures], FAILURE_TEXT_LENGTH, "%.24s [%.24s] %.140s", whenText, rule, text);
  }
  checks.failures = checks.failures + 1;
  tracef("FAIL", "[%s] %s", rule, text);
}

// Counts a rule that held, or records it as failed.
static void rule(bool held, const char *ruleName, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void rule(bool held, const char *ruleName, const char *format, ...) {
  if (held) {
    world->checks.checksRun = world->checks.checksRun + 1;
    return;
  }
  char text[FAILURE_TEXT_LENGTH];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  checkFail(ruleName, "%s", text);
}

static double toSeconds(uint64_t durationUs) {
  return (double)durationUs / (double)US_PER_SECOND_SIM;
}

// Returns how many of the recorded starts happened less than an hour before nowUs.
static uint32_t startsInLastHour(const uint64_t *startsUs, uint32_t startCount, uint64_t nowUs) {
  uint32_t stored = startCount;
  if (stored > START_RECORD_CAPACITY) {
    stored = START_RECORD_CAPACITY;
  }
  uint32_t recent = 0;
  for (uint32_t index = 0; index < stored; index++) {
    uint64_t startUs = startsUs[index];
    uint64_t ageUs = nowUs - startUs;
    if (ageUs < SPEC_START_WINDOW_US) {
      recent = recent + 1;
    }
  }
  return recent;
}

static void recordStartTime(uint64_t *startsUs, uint32_t &startCount, uint64_t nowUs) {
  uint32_t slot = startCount % START_RECORD_CAPACITY;
  startsUs[slot] = nowUs;
  startCount = startCount + 1;
}

// The firmware's settings and sensor state, read straight from it (only while it runs).
static int firmwareMode() {
  return settings.hvacMode;
}

static int firmwareFanMode() {
  return settings.fanMode;
}

static int firmwareSensorState() {
  return currentSensorState();
}

// Checks one relay contact change as it happens (called from the GPIO write).
void checkRelayEdge(uint8_t pin, bool closed) {
  CheckState &checks = world->checks;
  uint64_t nowUs = world->nowUs;
  bool w1 = simRelayClosed(SIM_PIN_W1);
  bool y1 = simRelayClosed(SIM_PIN_Y1);
  bool g = simRelayClosed(SIM_PIN_G);
  if (!checks.checksEnabled) {
    checks.w1 = w1;
    checks.y1 = y1;
    checks.g = g;
    return;
  }
  uint64_t uptimeUs = simUptimeUs();
  int mode = firmwareMode();
  int sensorState = firmwareSensorState();
  bool sensorFresh = (sensorState == SIM_SENSOR_OK);
  rule(!(w1 && y1), "W1+Y1", "W1 and Y1 closed at the same time");

  if (pin == SIM_PIN_Y1 && closed) {
    uint64_t offUs = nowUs - checks.y1FallUs;
    uint64_t sinceHeatUs = nowUs - checks.w1FallUs;
    rule(g, "G before Y1", "Y1 closed while G was open (the blower must already run)");
    rule(uptimeUs + SPEC_TIMING_TOLERANCE_US >= SPEC_POWER_UP_DELAY_US, "power-up delay",
         "Y1 closed %.1f s after boot (300 s required)", toSeconds(uptimeUs));
    rule(offUs + SPEC_TIMING_TOLERANCE_US >= SPEC_COMPRESSOR_MIN_OFF_US, "compressor min off",
         "Y1 closed after only %.1f s off (300 s required)", toSeconds(offUs));
    rule(sinceHeatUs + SPEC_TIMING_TOLERANCE_US >= SPEC_DEAD_BAND_US, "dead band",
         "Y1 closed %.1f s after W1 opened (300 s required)", toSeconds(sinceHeatUs));
    rule(mode == SIM_MODE_COOL, "cool mode", "Y1 closed in hvac mode %d", mode);
    rule(sensorFresh, "sensor", "Y1 closed with sensor state %d (only on a fresh reading)", sensorState);
    uint32_t recentStarts = startsInLastHour(checks.coolStartsUs, checks.coolStartCount, nowUs);
    uint32_t thisStart = recentStarts + 1;
    rule(thisStart <= SPEC_COOL_STARTS_PER_HOUR, "cool starts/hour", "cool start %u within one hour (4 allowed)", thisStart);
    if (thisStart > checks.maxCoolStartsPerHour) {
      checks.maxCoolStartsPerHour = thisStart;
    }
    recordStartTime(checks.coolStartsUs, checks.coolStartCount, nowUs);
    checks.y1RiseUs = nowUs;
    world->day.coolStarts = world->day.coolStarts + 1;
    world->total.coolStarts = world->total.coolStarts + 1;
  }

  if (pin == SIM_PIN_Y1 && !closed) {
    uint64_t onUs = nowUs - checks.y1RiseUs;
    rule(onUs <= SPEC_COOL_MAX_RUN_US + SPEC_TIMING_TOLERANCE_US, "cool run limit", "Y1 ran %.1f s in one run (6 h allowed)", toSeconds(onUs));
    bool modeOff = (mode == SIM_MODE_OFF);
    bool sensorSafetyStop = (sensorState == SIM_SENSOR_FAILED) || (sensorState == SIM_SENSOR_LOCKOUT);
    bool safetyStop = modeOff || sensorSafetyStop;
    if (!safetyStop) {
      rule(onUs + SPEC_TIMING_TOLERANCE_US >= SPEC_COMPRESSOR_MIN_ON_US, "compressor min on",
           "Y1 opened after only %.1f s on (300 s required)", toSeconds(onUs));
    }
    checks.coolStopNormal = !safetyStop;
    checks.y1FallUs = nowUs;
  }

  if (pin == SIM_PIN_W1 && closed) {
    uint64_t sinceCoolUs = nowUs - checks.y1FallUs;
    bool sensorRead = (sensorState == SIM_SENSOR_OK) || (sensorState == SIM_SENSOR_FAILED);
    rule(uptimeUs + SPEC_TIMING_TOLERANCE_US >= SPEC_POWER_UP_DELAY_US, "power-up delay",
         "W1 closed %.1f s after boot (300 s required)", toSeconds(uptimeUs));
    rule(sinceCoolUs + SPEC_TIMING_TOLERANCE_US >= SPEC_DEAD_BAND_US, "dead band",
         "W1 closed %.1f s after Y1 opened (300 s required)", toSeconds(sinceCoolUs));
    rule(mode == SIM_MODE_HEAT, "heat mode", "W1 closed in hvac mode %d", mode);
    rule(sensorRead, "sensor", "W1 closed with sensor state %d (only on a fresh reading or the fallback)", sensorState);
    uint32_t recentStarts = startsInLastHour(checks.heatStartsUs, checks.heatStartCount, nowUs);
    uint32_t thisStart = recentStarts + 1;
    rule(thisStart <= SPEC_HEAT_STARTS_PER_HOUR, "heat starts/hour", "heat start %u within one hour (6 allowed)", thisStart);
    if (thisStart > checks.maxHeatStartsPerHour) {
      checks.maxHeatStartsPerHour = thisStart;
    }
    recordStartTime(checks.heatStartsUs, checks.heatStartCount, nowUs);
    checks.w1RiseUs = nowUs;
    world->day.heatStarts = world->day.heatStarts + 1;
    world->total.heatStarts = world->total.heatStarts + 1;
  }

  if (pin == SIM_PIN_W1 && !closed) {
    uint64_t onUs = nowUs - checks.w1RiseUs;
    rule(onUs <= SPEC_HEAT_MAX_RUN_US + SPEC_TIMING_TOLERANCE_US, "heat run limit", "W1 ran %.1f s in one run (3 h allowed)", toSeconds(onUs));
    bool safetyStop = (mode == SIM_MODE_OFF) || (sensorState == SIM_SENSOR_LOCKOUT);
    if (!safetyStop) {
      rule(onUs + SPEC_TIMING_TOLERANCE_US >= SPEC_FURNACE_MIN_ON_US, "furnace min on",
           "W1 opened after only %.1f s on (180 s required)", toSeconds(onUs));
    }
    checks.w1FallUs = nowUs;
  }

  if (pin == SIM_PIN_G && !closed) {
    uint64_t sinceCoolUs = nowUs - checks.y1FallUs;
    rule(!y1, "G with Y1", "G opened while Y1 was closed (the compressor must stop first)");
    bool withinRunOn = (sinceCoolUs + SPEC_TIMING_TOLERANCE_US < SPEC_FAN_RUN_ON_US);
    bool runOnExpected = checks.coolStopNormal && withinRunOn && (mode != SIM_MODE_OFF) && (sensorState != SIM_SENSOR_LOCKOUT);
    rule(!runOnExpected, "fan run-on", "G opened %.1f s after a cool cycle ended (60 s run-on required)", toSeconds(sinceCoolUs));
    bool afterRunOn = !withinRunOn && (sinceCoolUs < RUN_ON_OBSERVED_MAX_US);
    if (checks.coolStopNormal && afterRunOn) {
      checks.runOnsObserved = checks.runOnsObserved + 1;
    }
  }
  checks.w1 = w1;
  checks.y1 = y1;
  checks.g = g;
}

// Records what a reset does to the relays: every contact opens. A Wi-Fi restart must wait until
// heat and cool are off.
void checkResetEdges() {
  CheckState &checks = world->checks;
  uint64_t nowUs = world->nowUs;
  bool wifiRestart = (world->resetKind == RESET_WATCHDOG_REBOOT);
  if (wifiRestart && checks.checksEnabled) {
    rule(!checks.w1 && !checks.y1, "Wi-Fi restart", "the Wi-Fi restart happened with W1 %s and Y1 %s",
         checks.w1 ? "closed" : "open", checks.y1 ? "closed" : "open");
  }
  if (checks.w1) {
    checks.w1FallUs = nowUs;
    tracef("RESET", "W1 opened by the reset after %.0f s on", toSeconds(nowUs - checks.w1RiseUs));
  }
  if (checks.y1) {
    checks.y1FallUs = nowUs;
    checks.coolStopNormal = false;
    tracef("RESET", "Y1 opened by the reset after %.0f s on", toSeconds(nowUs - checks.y1RiseUs));
  }
  if (checks.g) {
    tracef("RESET", "G opened by the reset");
  }
  checks.w1 = false;
  checks.y1 = false;
  checks.g = false;
  checks.commitSeenThisBoot = false;
}

// Child-local: the interlock override is reported once per boot.
static bool interlockOverrideReported = false;

// The rules that hold after every pass of core 1's loop.
void checkAfterCore1Pass() {
  CheckState &checks = world->checks;
  uint64_t nowUs = world->nowUs;
  int mode = firmwareMode();
  int fanMode = firmwareFanMode();
  int sensorState = firmwareSensorState();
  bool w1 = simRelayClosed(SIM_PIN_W1);
  bool y1 = simRelayClosed(SIM_PIN_Y1);
  bool g = simRelayClosed(SIM_PIN_G);
  bool settingsChanged = (mode != world->lastSeenMode) || ((int)settings.setpointF != world->lastSeenSetpointF);
  if (settingsChanged) {
    world->settingsChangedUs = nowUs;
    world->lastSeenMode = mode;
    world->lastSeenSetpointF = settings.setpointF;
  }
  world->lastSeenSensorState = sensorState;
  if (sensorState != SIM_SENSOR_OK) {
    world->sensorProblemUs = nowUs;
  }
  if (!checks.checksEnabled) {
    return;
  }
  rule(simMutexDepth() == 0, "mutex", "stateMutex still held (depth %d) after core 1's pass", simMutexDepth());
  rule(simInterruptsEnabled(), "interrupts", "interrupts left disabled after core 1's pass");
  RelayOutputs firmwareRelays = currentRelays();
  bool relaysMatch = (firmwareRelays.heatW1 == w1) && (firmwareRelays.coolY1 == y1) && (firmwareRelays.fanG == g);
  rule(relaysMatch, "relay state", "firmware reports W1 %d Y1 %d G %d but the pins drive W1 %d Y1 %d G %d",
       firmwareRelays.heatW1, firmwareRelays.coolY1, firmwareRelays.fanG, w1, y1, g);
  if (sensorState == SIM_SENSOR_LOCKOUT) {
    rule(!w1 && !y1 && !g, "sensor lockout", "a relay is closed during the startup lockout (W1 %d Y1 %d G %d)", w1, y1, g);
  }
  if (sensorState == SIM_SENSOR_STALE) {
    uint64_t heatOnUs = nowUs - checks.w1RiseUs;
    uint64_t coolOnUs = nowUs - checks.y1RiseUs;
    if (w1) {
      rule(heatOnUs < SPEC_FURNACE_MIN_ON_US + STALE_STOP_TOLERANCE_US, "stale sensor",
           "the furnace still runs %.1f s into its cycle on a stale reading", toSeconds(heatOnUs));
    }
    if (y1) {
      rule(coolOnUs < SPEC_COMPRESSOR_MIN_ON_US + STALE_STOP_TOLERANCE_US, "stale sensor",
           "the compressor still runs %.1f s into its cycle on a stale reading", toSeconds(coolOnUs));
    }
  }
  if (sensorState == SIM_SENSOR_FAILED) {
    rule(!y1, "failed sensor", "Y1 closed with a failed sensor");
    rule(mode != SIM_MODE_COOL, "failed sensor", "cool mode still selected with a failed sensor");
  }
  if (mode == SIM_MODE_OFF) {
    rule(!w1 && !y1, "mode off", "W1 %d / Y1 %d closed in mode off", w1, y1);
  }
  if (fanMode == SIM_FAN_ON && sensorState != SIM_SENSOR_LOCKOUT) {
    rule(g, "fan on", "G open in fan-on mode");
  }
  if (fanMode == SIM_FAN_AUTO) {
    rule(!(w1 && g), "fan auto heat", "G closed with W1 in fan auto (the furnace runs its own blower)");
    uint64_t sinceCoolUs = nowUs - checks.y1FallUs;
    bool withinRunOn = (sinceCoolUs < SPEC_FAN_RUN_ON_US + PASS_TOLERANCE_US);
    bool runOnAllowed = checks.coolStopNormal && withinRunOn && (mode != SIM_MODE_OFF);
    if (!y1 && !runOnAllowed) {
      rule(!g, "fan auto idle", "G closed in fan auto with no cooling and no run-on (%.1f s after Y1 opened)",
           toSeconds(sinceCoolUs));
    }
  }
  bool overridden = interlocksHaveOverridden();
  if (overridden && !interlockOverrideReported) {
    interlockOverrideReported = true;
    checkFail("interlocks", "the final interlock had to override the thermostat's relay request (see the FW lines)");
  } else if (!overridden) {
    checks.checksRun = checks.checksRun + 1;
  }
}

// One EEPROM write: at most one per 5 minutes, and boot counts as a write.
void checkEepromCommit() {
  CheckState &checks = world->checks;
  uint64_t uptimeUs = simUptimeUs();
  if (!checks.checksEnabled) {
    return;
  }
  bool bootWipe = (uptimeUs < US_PER_SECOND_SIM) && !checks.commitSeenThisBoot;
  if (checks.commitSeenThisBoot) {
    uint64_t sinceLastUs = uptimeUs - checks.lastCommitUptimeUs;
    rule(sinceLastUs + SPEC_TIMING_TOLERANCE_US >= SPEC_EEPROM_MIN_INTERVAL_US, "EEPROM interval",
         "EEPROM written %.1f s after the previous write (300 s required)", toSeconds(sinceLastUs));
  } else if (!bootWipe) {
    rule(uptimeUs + SPEC_TIMING_TOLERANCE_US >= SPEC_EEPROM_MIN_INTERVAL_US, "EEPROM interval",
         "first EEPROM write %.1f s after boot (boot counts as a write, 300 s required)", toSeconds(uptimeUs));
  }
  checks.lastCommitUptimeUs = uptimeUs;
  checks.commitSeenThisBoot = true;
}

// Interrupts may be held off only briefly (the DHT22 read).
void checkInterruptsOff(uint64_t offUs) {
  CheckState &checks = world->checks;
  if (offUs > checks.maxInterruptsOffUs) {
    checks.maxInterruptsOffUs = offUs;
  }
  rule(offUs <= SPEC_MAX_INTERRUPTS_OFF_US, "interrupts", "interrupts held off %.2f ms", (double)offUs / 1000.0);
}

// ==== statistics ====

static void addSample(RunStats &stats, uint64_t durationUs, bool w1, bool y1, bool g, bool comfortApplies, bool comfortMissed) {
  HouseState &house = world->house;
  if (stats.samples == 0) {
    stats.airMinF = house.airF;
    stats.airMaxF = house.airF;
    stats.outdoorMinF = house.outdoorF;
    stats.outdoorMaxF = house.outdoorF;
    stats.humidityMin = house.relativeHumidity;
    stats.humidityMax = house.relativeHumidity;
  }
  stats.samples = stats.samples + 1;
  stats.airSumF = stats.airSumF + house.airF;
  stats.airMinF = fmin(stats.airMinF, house.airF);
  stats.airMaxF = fmax(stats.airMaxF, house.airF);
  stats.outdoorMinF = fmin(stats.outdoorMinF, house.outdoorF);
  stats.outdoorMaxF = fmax(stats.outdoorMaxF, house.outdoorF);
  stats.humidityMin = fmin(stats.humidityMin, house.relativeHumidity);
  stats.humidityMax = fmax(stats.humidityMax, house.relativeHumidity);
  if (w1) {
    stats.w1Us = stats.w1Us + durationUs;
  }
  if (y1) {
    stats.y1Us = stats.y1Us + durationUs;
  }
  if (g) {
    stats.gUs = stats.gUs + durationUs;
  }
  if (comfortApplies) {
    stats.comfortCheckedUs = stats.comfortCheckedUs + durationUs;
    if (comfortMissed) {
      stats.comfortMissedUs = stats.comfortMissedUs + durationUs;
    }
  }
}

// Adds the step from fromUs to toUs to the day's and the run's statistics, with the comfort
// rule: once settings, a restart and the sensor have been settled for 90 minutes, the room must
// not be more than 2 F colder than the setpoint in heat (or warmer in cool) while the furnace (or
// compressor) sits idle. Out of band with the equipment running is its capacity, not the control.
void statsAdvance(uint64_t fromUs, uint64_t toUs) {
  uint64_t durationUs = toUs - fromUs;
  bool w1 = simRelayClosed(SIM_PIN_W1);
  bool y1 = simRelayClosed(SIM_PIN_Y1);
  bool g = simRelayClosed(SIM_PIN_G);
  int mode = world->lastSeenMode;
  bool modeActive = (mode == SIM_MODE_HEAT) || (mode == SIM_MODE_COOL);
  bool settingsSettled = (toUs - world->settingsChangedUs) >= COMFORT_SETTLE_US;
  bool restartSettled = (toUs - world->lastResetWorldUs) >= COMFORT_SETTLE_US;
  bool sensorSettled = (toUs - world->sensorProblemUs) >= COMFORT_SETTLE_US;
  bool comfortApplies = world->firmwareUp && modeActive && settingsSettled && restartSettled && sensorSettled;
  bool comfortMissed = false;
  if (comfortApplies) {
    double setpointF = (double)world->lastSeenSetpointF;
    double airF = world->house.airF;
    if (mode == SIM_MODE_HEAT) {
      bool tooCold = (airF < setpointF - COMFORT_BAND_F);
      comfortMissed = tooCold && !w1;
    } else {
      bool tooHot = (airF > setpointF + COMFORT_BAND_F);
      comfortMissed = tooHot && !y1;
    }
  }
  addSample(world->day, durationUs, w1, y1, g, comfortApplies, comfortMissed);
  addSample(world->total, durationUs, w1, y1, g, comfortApplies, comfortMissed);
}

static void formatDuration(char *output, size_t outputSize, uint64_t durationUs) {
  uint64_t totalMinutes = durationUs / US_PER_MINUTE_SIM;
  unsigned long hours = (unsigned long)(totalMinutes / 60);
  unsigned long minutes = (unsigned long)(totalMinutes % 60);
  snprintf(output, outputSize, "%luh%02lum", hours, minutes);
}

static double percentOf(uint64_t part, uint64_t whole) {
  if (whole == 0) {
    return 0.0;
  }
  return 100.0 * (double)part / (double)whole;
}

// Writes the summary lines of one day (or the whole run) to the trace.
static void traceStats(const char *label, const RunStats &stats) {
  uint64_t sampledUs = 0;
  if (stats.samples > 0) {
    sampledUs = stats.samples * (uint64_t)world->config.stepMs * US_PER_MS_SIM;
  }
  double airAverageF = (stats.samples > 0) ? stats.airSumF / (double)stats.samples : 0.0;
  char heatText[16];
  char coolText[16];
  char fanText[16];
  formatDuration(heatText, sizeof(heatText), stats.w1Us);
  formatDuration(coolText, sizeof(coolText), stats.y1Us);
  formatDuration(fanText, sizeof(fanText), stats.gUs);
  tracef("DAY", "%s (%s): outdoor %.1f..%.1f F, indoor air %.1f..%.1f F (average %.1f), RH %.0f..%.0f %%", label,
         seasonName(world->config.season), stats.outdoorMinF, stats.outdoorMaxF, stats.airMinF, stats.airMaxF,
         airAverageF, stats.humidityMin, stats.humidityMax);
  tracef("DAY", "%s: heat W1 %s (%.0f %%), %u starts | cool Y1 %s (%.0f %%), %u starts | blower G %s", label, heatText,
         percentOf(stats.w1Us, sampledUs), stats.heatStarts, coolText, percentOf(stats.y1Us, sampledUs), stats.coolStarts,
         fanText);
  double comfortPercent = 100.0 - percentOf(stats.comfortMissedUs, stats.comfortCheckedUs);
  char comfortCheckedText[16];
  formatDuration(comfortCheckedText, sizeof(comfortCheckedText), stats.comfortCheckedUs);
  tracef("DAY", "%s: comfort %.1f %% of %s settled time | DHT22 %llu reads, %llu faulted | EEPROM %u writes | "
         "firmware log %u lines | API %u requests | boots %u", label, comfortPercent, comfortCheckedText,
         (unsigned long long)stats.sensorReads, (unsigned long long)stats.sensorFaultedReads, stats.eepromCommits,
         stats.firmwareLogLines, stats.apiRequests, stats.boots);
}

// Ends a day: writes its summary and starts the next. The last call also writes the run's totals.
void statsFinishDay(bool finalDay) {
  char label[24];
  snprintf(label, sizeof(label), "day %u", world->dayNumber);
  traceStats(label, world->day);
  memset(&world->day, 0, sizeof(world->day));
  if (finalDay) {
    traceStats("run total", world->total);
    return;
  }
  world->dayNumber = world->dayNumber + 1;
}

static const char *modeText(int mode) {
  if (mode == SIM_MODE_HEAT) {
    return "heat";
  }
  if (mode == SIM_MODE_COOL) {
    return "cool";
  }
  if (mode == SIM_MODE_OFF) {
    return "off";
  }
  return "?";
}

static const char *sensorText(int state) {
  const char *names[] = { "ok", "starting", "startup lockout", "stale", "failed" };
  if (state < 0 || state > SIM_SENSOR_FAILED) {
    return "?";
  }
  return names[state];
}

// Writes a WORLD line: weather, house, settings, relays and sensor state.
void writeSnapshot() {
  HouseState &house = world->house;
  bool w1 = simRelayClosed(SIM_PIN_W1);
  bool y1 = simRelayClosed(SIM_PIN_Y1);
  bool g = simRelayClosed(SIM_PIN_G);
  const char *running = "idle";
  if (w1) {
    running = "heating";
  } else if (y1) {
    running = "cooling";
  } else if (g) {
    running = "fan only";
  }
  if (!world->firmwareUp) {
    tracef("WORLD", "outdoor %.1f F | indoor air %.2f F, structure %.2f F, at sensor %.2f F, RH %.1f %% | Pico off, "
           "relays open", house.outdoorF, house.airF, house.massF, house.sensorF, house.relativeHumidity);
    return;
  }
  tracef("WORLD", "outdoor %.1f F | indoor air %.2f F, structure %.2f F, at sensor %.2f F, RH %.1f %% | mode %s, fan %s, "
         "setpoint %d F | W1 %s, Y1 %s, G %s (%s) | sensor %s | furnace %.0f %%, coil %.0f %%",
         house.outdoorF, house.airF, house.massF, house.sensorF, house.relativeHumidity, modeText(world->lastSeenMode),
         (settings.fanMode == SIM_FAN_ON) ? "on" : "auto", world->lastSeenSetpointF, w1 ? "closed" : "open",
         y1 ? "closed" : "open", g ? "closed" : "open", running, sensorText(world->lastSeenSensorState),
         house.furnaceOutput * 100.0, house.coolingOutput * 100.0);
}

// Writes one CSV row (one per simulated minute), for plotting.
void writeCsvRow() {
  if (csvFile == nullptr) {
    return;
  }
  HouseState &house = world->house;
  char whenText[32];
  formatWorldTime(whenText, sizeof(whenText), world->nowUs);
  double minutes = (double)world->nowUs / (double)US_PER_MINUTE_SIM;
  int fanMode = world->firmwareUp ? settings.fanMode : -1;
  int mode = world->firmwareUp ? world->lastSeenMode : -1;
  int setpointF = world->firmwareUp ? world->lastSeenSetpointF : -1;
  int sensorState = world->firmwareUp ? world->lastSeenSensorState : -1;
  fprintf(csvFile, "%.2f,%s,%.2f,%.3f,%.3f,%.3f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d\n", minutes, whenText, house.outdoorF,
          house.airF, house.massF, house.sensorF, house.relativeHumidity, setpointF, mode, fanMode,
          simRelayClosed(SIM_PIN_W1) ? 1 : 0, simRelayClosed(SIM_PIN_Y1) ? 1 : 0, simRelayClosed(SIM_PIN_G) ? 1 : 0,
          sensorState, world->firmwareUp ? 1 : 0);
}
