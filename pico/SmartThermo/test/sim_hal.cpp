// =====================================================================================
// test/sim_hal.cpp
// SmartThermo host test: the hardware under the firmware. A simulated clock, the GPIO pins (relay
// writes are traced and checked as they happen), the DHT22 data line answering the firmware's
// start signal with a real pulse train, the EEPROM flash, the Wi-Fi access point, the watchdog,
// the core mutex, and the HTTP reply buffers that replace http.cpp.
//
// Two cores: the runner calls loop1() (core 1) and loop() (core 0) in turn once per step. A
// delay() marks the calling core busy until it ends, and a Wi-Fi join holds core 0 like the real
// one, while the other core carries on.
// =====================================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>
#include <pico/mutex.h>
#include <math.h>
#include <stdarg.h>

#include "../http.h"
#include "../display.h"
#include "sim_world.h"

World *world = nullptr;
FILE *traceFile = nullptr;
FILE *csvFile = nullptr;

// ---- DHT22 line timing (datasheet), microseconds after the host releases the line ----
const uint64_t DHT_ANSWER_DELAY_US = 30;   // the sensor waits 20-40 us, then pulls low
const uint64_t DHT_ANSWER_LOW_END_US = 110; // 80 us low
const uint64_t DHT_ANSWER_HIGH_END_US = 190; // 80 us high, then the first bit
const uint64_t DHT_END_LOW_US = 50;          // low after the last bit, then the line is released
const uint64_t DHT_MIN_START_LOW_US = 1000;  // the host must hold the line low at least 1 ms
const uint8_t DHT_FRAME_BITS = 40;
const uint32_t DHT_MAX_EDGES = 2 * DHT_FRAME_BITS + 8;

// ---- Wi-Fi timing ----
const uint64_t WIFI_JOIN_US = 1200000;     // a join to a present access point holds core 0 this long
const uint64_t WIFI_ADDRESS_US = 2500000;  // then DHCP gives an address this long after the join started

// ---- HTTP reply buffers, sized as in http.cpp ----
const uint8_t SIM_COMMAND_REPLY_LENGTH = 48;
const uint16_t SIM_HTTP_BODY_BUFFER_SIZE = 6144;

const uint32_t RECENT_WRITE_CAPACITY = 32;

// state that belongs to one boot (a forked child), reset by simBeginBoot()
struct HalBootState {
  int currentCore;
  uint64_t core0BusyUntilUs;
  uint64_t core1BusyUntilUs;
  uint64_t wifiTimeoutMs;
  bool dhtStartLowSeen;
  uint64_t dhtStartLowUs;
  bool dhtReplying;
  uint64_t dhtReleaseUs;
  uint8_t dhtFrame[5];
  SensorFault dhtFault;
  uint64_t dhtEdgeUs[DHT_MAX_EDGES];  // the line's waveform: from dhtEdgeUs[n] (after release) it is dhtEdgeLevel[n]
  int dhtEdgeLevel[DHT_MAX_EDGES];
  uint32_t dhtEdgeCount;
  uint32_t dhtEdgeIndex;               // the segment the line is in; reads only move forward in time
  bool dhtFrameForced;
  uint8_t forcedFrame[5];
  SensorFault forcedFault;
  bool interruptsOff;
  uint64_t interruptsOffSinceUs;
  recursive_mutex_t *knownMutex;
  uint8_t recentWritePins[RECENT_WRITE_CAPACITY];
  int recentWriteLevels[RECENT_WRITE_CAPACITY];
  uint32_t recentWriteCount;
};
static HalBootState hal;

// ==== clock ====

// Returns the time since the current boot in microseconds.
uint64_t simUptimeUs() {
  uint64_t uptimeUs = world->nowUs - world->bootUs;
  return uptimeUs;
}

// Returns the time since the current boot in whole milliseconds.
uint64_t simUptimeMs() {
  uint64_t uptimeMs = simUptimeUs() / US_PER_MS_SIM;
  return uptimeMs;
}

uint64_t time_us_64() {
  return simUptimeUs();
}

// Each call costs one microsecond of CPU time, so the firmware's busy-wait loops (the DHT22
// pulse timing) see time pass as they would on the RP2040.
uint32_t time_us_32() {
  world->nowUs = world->nowUs + 1;
  uint64_t uptimeUs = simUptimeUs();
  return (uint32_t)uptimeUs;
}

uint32_t millis() {
  uint64_t uptimeMs = simUptimeMs();
  return (uint32_t)uptimeMs;
}

// A delay keeps the calling core busy; the other core carries on.
void delay(uint32_t milliseconds) {
  uint64_t delayUs = (uint64_t)milliseconds * US_PER_MS_SIM;
  uint64_t untilUs = world->nowUs + delayUs;
  uint64_t &busyUntilUs = (hal.currentCore == 1) ? hal.core1BusyUntilUs : hal.core0BusyUntilUs;
  if (untilUs > busyUntilUs) {
    busyUntilUs = untilUs;
  }
}

// A busy-wait: the clock moves on.
void delayMicroseconds(uint32_t microseconds) {
  world->nowUs = world->nowUs + microseconds;
}

// A busy-wait: the clock moves on.
void busy_wait_us_32(uint32_t delayUs) {
  world->nowUs = world->nowUs + delayUs;
}

// Clears the state of the previous boot; called by the runner at the start of every boot.
void simBeginBoot() {
  memset(&hal, 0, sizeof(hal));
  hal.wifiTimeoutMs = 15000; // the library default until the firmware sets its own
}

// Selects the core the next firmware calls come from (0 = loop(), 1 = loop1()).
void simSetCurrentCore(int core) {
  hal.currentCore = core;
}

// Returns false while a core is inside a delay, a Wi-Fi join or a simulated hang.
bool simCoreMayRun(int core) {
  uint64_t nowUs = world->nowUs;
  if (core == 1) {
    bool busy = (nowUs < hal.core1BusyUntilUs);
    bool hung = (nowUs < world->core1StallUntilUs);
    return !busy && !hung;
  }
  bool busy = (nowUs < hal.core0BusyUntilUs);
  bool hung = (nowUs < world->core0StallUntilUs);
  return !busy && !hung;
}

// ==== random numbers (repeatable for a seed, independent of the order of calls) ====

static uint64_t mix64(uint64_t value) {
  uint64_t mixed = value + 0x9E3779B97F4A7C15ull;
  mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
  mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
  mixed = mixed ^ (mixed >> 31);
  return mixed;
}

// Returns a number from 0 up to 1 for the given keys, e.g. simRandom(day-weather, day, 0).
double simRandom(uint64_t first, uint64_t second, uint64_t third) {
  uint64_t thirdMixed = mix64(third);
  uint64_t secondMixed = mix64(second ^ thirdMixed);
  uint64_t firstMixed = mix64(first ^ secondMixed);
  uint64_t hash = mix64((uint64_t)world->config.seed ^ firstMixed);
  uint64_t top53Bits = hash >> 11;
  double fraction = (double)top53Bits / 9007199254740992.0;
  return fraction;
}

// Returns a normally distributed number (mean 0, deviation 1) for the given keys.
double simGaussian(uint64_t first, uint64_t second, uint64_t third) {
  uint64_t firstDraw = third * 2 + 1;
  uint64_t secondDraw = third * 2 + 2;
  double uniformA = simRandom(first, second, firstDraw);
  double uniformB = simRandom(first, second, secondDraw);
  if (uniformA < 1e-12) {
    uniformA = 1e-12;
  }
  double radius = sqrt(-2.0 * log(uniformA));
  double angle = 2.0 * SIM_PI * uniformB;
  return radius * cos(angle);
}

// ==== trace ====

// Formats a world time as "d2 06:30:05.250" (day from 1, time of day).
void formatWorldTime(char *output, size_t outputSize, uint64_t worldUs) {
  double startHour = world->config.startHour;
  uint64_t startOffsetMs = (uint64_t)(startHour * 3600000.0 + 0.5);
  uint64_t totalMs = worldUs / US_PER_MS_SIM + startOffsetMs;
  uint64_t dayIndex = totalMs / 86400000ull;
  uint64_t msIntoDay = totalMs % 86400000ull;
  unsigned long hours = (unsigned long)(msIntoDay / 3600000ull);
  unsigned long minutes = (unsigned long)((msIntoDay / 60000ull) % 60);
  unsigned long seconds = (unsigned long)((msIntoDay / 1000ull) % 60);
  unsigned long milliseconds = (unsigned long)(msIntoDay % 1000ull);
  unsigned long dayNumber = (unsigned long)(dayIndex + 1);
  snprintf(output, outputSize, "d%lu %02lu:%02lu:%02lu.%03lu", dayNumber, hours, minutes, seconds, milliseconds);
}

// Writes one trace line: world time, uptime of the running boot (or "power off"), category, text.
void tracef(const char *category, const char *format, ...) {
  if (traceFile == nullptr) {
    return;
  }
  char worldText[32];
  formatWorldTime(worldText, sizeof(worldText), world->nowUs);
  char uptimeText[24];
  if (world->firmwareUp) {
    uint64_t uptimeSeconds = simUptimeUs() / US_PER_SECOND_SIM;
    unsigned long hours = (unsigned long)(uptimeSeconds / 3600);
    unsigned long minutes = (unsigned long)((uptimeSeconds / 60) % 60);
    unsigned long seconds = (unsigned long)(uptimeSeconds % 60);
    snprintf(uptimeText, sizeof(uptimeText), "up %lu:%02lu:%02lu", hours, minutes, seconds);
  } else {
    snprintf(uptimeText, sizeof(uptimeText), "power off");
  }
  fprintf(traceFile, "%s  %-12s %-7s ", worldText, uptimeText, category);
  va_list arguments;
  va_start(arguments, format);
  vfprintf(traceFile, format, arguments);
  va_end(arguments);
  fputc('\n', traceFile);
}

// ==== GPIO ====

static bool isRelayPin(uint8_t pin) {
  bool relayPin = (pin == SIM_PIN_W1) || (pin == SIM_PIN_Y1) || (pin == SIM_PIN_G);
  return relayPin;
}

// Returns the relay's name and what it switches, e.g. "W1 (furnace heat call)".
static const char *relayDescription(uint8_t pin) {
  if (pin == SIM_PIN_W1) {
    return "W1 (furnace heat call)";
  }
  if (pin == SIM_PIN_Y1) {
    return "Y1 (compressor)";
  }
  return "G (indoor blower)";
}

// Returns true while a relay's contact is closed: its pin is an output driven HIGH.
bool simRelayClosed(uint8_t pin) {
  const PinSim &pinState = world->pins[pin];
  bool closed = (pinState.mode == OUTPUT) && (pinState.level == HIGH);
  return closed;
}

static const char *modeName(int mode) {
  if (mode == OUTPUT) {
    return "OUTPUT";
  }
  if (mode == INPUT_PULLUP) {
    return "INPUT_PULLUP";
  }
  return "INPUT";
}

static const char *driveStrengthName(int driveStrength) {
  if (driveStrength == GPIO_DRIVE_STRENGTH_2MA) {
    return "2 mA";
  }
  if (driveStrength == GPIO_DRIVE_STRENGTH_8MA) {
    return "8 mA";
  }
  if (driveStrength == GPIO_DRIVE_STRENGTH_12MA) {
    return "12 mA";
  }
  return "4 mA";
}

// Records a relay pin change, traces it with all three contacts and checks the safety rules on
// a contact change, e.g. "GP17 Y1 (compressor) <- HIGH: contact closed | W1 open, Y1 closed, G closed".
static void changeRelayPin(uint8_t pin, int mode, int level, const char *action) {
  bool wasClosed = simRelayClosed(pin);
  world->pins[pin].mode = mode;
  world->pins[pin].level = level;
  bool isClosed = simRelayClosed(pin);
  const char *contactText = isClosed ? "contact closed" : "contact open";
  if (wasClosed == isClosed) {
    contactText = isClosed ? "contact stays closed" : "contact stays open";
  }
  const char *w1Text = simRelayClosed(SIM_PIN_W1) ? "closed" : "open";
  const char *y1Text = simRelayClosed(SIM_PIN_Y1) ? "closed" : "open";
  const char *gText = simRelayClosed(SIM_PIN_G) ? "closed" : "open";
  tracef("GPIO", "GP%u %s %s: %s | W1 %s, Y1 %s, G %s", pin, relayDescription(pin), action, contactText,
         w1Text, y1Text, gText);
  if (hal.recentWriteCount < RECENT_WRITE_CAPACITY) {
    hal.recentWritePins[hal.recentWriteCount] = pin;
    hal.recentWriteLevels[hal.recentWriteCount] = isClosed ? HIGH : LOW;
    hal.recentWriteCount = hal.recentWriteCount + 1;
  }
  if (wasClosed != isClosed) {
    checkRelayEdge(pin, isClosed);
  }
  bool weakDrive = world->pins[pin].driveStrength != GPIO_DRIVE_STRENGTH_12MA;
  if (isClosed && !wasClosed && weakDrive) {
    checkFail("gpio", "GP%u %s closed at %s drive; the 2N5551 base needs 12 mA", pin, relayDescription(pin),
              driveStrengthName(world->pins[pin].driveStrength));
  }
}

// Resets every pin as a reset does: input, no drive, pad at 4 mA. The relay drivers' pull-downs
// open every contact.
void simResetPins() {
  for (uint8_t pin = 0; pin < SIM_PIN_COUNT; pin++) {
    world->pins[pin].mode = INPUT;
    world->pins[pin].level = LOW;
    world->pins[pin].driveStrength = GPIO_DRIVE_STRENGTH_4MA;
  }
}

// Returns the drive strength last set on a pin, e.g. GPIO_DRIVE_STRENGTH_12MA after initRelays().
int simPinDriveStrength(uint8_t pin) {
  return world->pins[pin].driveStrength;
}

// Sets a pin's pad drive strength, traced for the relay pins: "GP18 W1 (furnace heat call) drive 12 mA".
void gpio_set_drive_strength(uint32_t gpio, enum gpio_drive_strength drive) {
  if (gpio >= SIM_PIN_COUNT) {
    checkFail("gpio", "gpio_set_drive_strength on pin %u, which does not exist", (unsigned)gpio);
    return;
  }
  world->pins[gpio].driveStrength = drive;
  if (isRelayPin((uint8_t)gpio)) {
    tracef("GPIO", "GP%u %s drive %s", (unsigned)gpio, relayDescription((uint8_t)gpio), driveStrengthName(drive));
  }
}

// ---- DHT22 data line ----

// Adds one segment to the waveform: the line is at `level` from `startUs` after the release.
static void addDhtEdge(uint64_t startUs, int level) {
  if (hal.dhtEdgeCount >= DHT_MAX_EDGES) {
    return;
  }
  hal.dhtEdgeUs[hal.dhtEdgeCount] = startUs;
  hal.dhtEdgeLevel[hal.dhtEdgeCount] = level;
  hal.dhtEdgeCount = hal.dhtEdgeCount + 1;
}

// Prepares the sensor's answer to a start signal: the frame (from the house, or forced by a unit
// test), the fault it carries, and the waveform: 20-40 us high, 80 us low, 80 us high, then per
// bit 48-55 us low and 24-28 us (0) or 68-72 us (1) high, 50 us low at the end, then released.
// Unplugged: always high. Shorted: low from the answer on. Data timeout: released part way.
static void beginDhtReply() {
  world->dht.reads = world->dht.reads + 1;
  SensorFault fault = SENSOR_FAULT_NONE;
  if (hal.dhtFrameForced) {
    memcpy(hal.dhtFrame, hal.forcedFrame, sizeof(hal.dhtFrame));
    fault = hal.forcedFault;
    hal.dhtFrameForced = false;
  } else {
    buildDhtFrame(hal.dhtFrame, fault);
  }
  hal.dhtFault = fault;
  if (fault != SENSOR_FAULT_NONE) {
    world->dht.faultedReads = world->dht.faultedReads + 1;
  }
  hal.dhtEdgeCount = 0;
  hal.dhtEdgeIndex = 0;
  addDhtEdge(0, HIGH);
  if (fault == SENSOR_FAULT_UNPLUGGED) {
    hal.dhtReleaseUs = world->nowUs;
    hal.dhtReplying = true;
    return;
  }
  addDhtEdge(DHT_ANSWER_DELAY_US, LOW);
  if (fault == SENSOR_FAULT_STUCK_LOW) {
    hal.dhtReleaseUs = world->nowUs;
    hal.dhtReplying = true;
    return;
  }
  addDhtEdge(DHT_ANSWER_LOW_END_US, HIGH);
  uint64_t readIndex = world->dht.reads;
  uint8_t stopBit = DHT_FRAME_BITS;
  if (fault == SENSOR_FAULT_DATA_TIMEOUT) {
    double stopDraw = simRandom(43, readIndex, 0);
    stopBit = 8 + (uint8_t)(stopDraw * 31.0);
  }
  uint64_t cursorUs = DHT_ANSWER_HIGH_END_US;
  for (uint8_t bitIndex = 0; bitIndex < stopBit; bitIndex++) {
    uint8_t byteIndex = bitIndex / 8;
    uint8_t bitInByte = 7 - (bitIndex % 8);
    bool bitIsOne = ((hal.dhtFrame[byteIndex] >> bitInByte) & 1) != 0;
    double lowJitter = simRandom(41, readIndex, bitIndex);
    double highJitter = simRandom(42, readIndex, bitIndex);
    uint64_t lowUs = 48 + (uint64_t)(lowJitter * 7.0);
    uint64_t highUs = bitIsOne ? 68 + (uint64_t)(highJitter * 5.0) : 24 + (uint64_t)(highJitter * 5.0);
    addDhtEdge(cursorUs, LOW);
    cursorUs = cursorUs + lowUs;
    addDhtEdge(cursorUs, HIGH);
    cursorUs = cursorUs + highUs;
  }
  bool frameComplete = (stopBit == DHT_FRAME_BITS);
  if (frameComplete) {
    addDhtEdge(cursorUs, LOW);
    addDhtEdge(cursorUs + DHT_END_LOW_US, HIGH);
  }
  hal.dhtReleaseUs = world->nowUs;
  hal.dhtReplying = true;
}

// Returns the DHT22 data line's level now: the pull-up (HIGH) unless the sensor drives it low.
static int dhtLineLevel() {
  if (!hal.dhtReplying) {
    return HIGH;
  }
  uint64_t sinceReleaseUs = world->nowUs - hal.dhtReleaseUs;
  while (hal.dhtEdgeIndex + 1 < hal.dhtEdgeCount && sinceReleaseUs >= hal.dhtEdgeUs[hal.dhtEdgeIndex + 1]) {
    hal.dhtEdgeIndex = hal.dhtEdgeIndex + 1;
  }
  return hal.dhtEdgeLevel[hal.dhtEdgeIndex];
}

// The next DHT22 answer carries this frame and fault instead of the house's reading (unit tests).
void simForceNextDhtFrame(const uint8_t frame[5], SensorFault fault) {
  memcpy(hal.forcedFrame, frame, sizeof(hal.forcedFrame));
  hal.forcedFault = fault;
  hal.dhtFrameForced = true;
}

void pinMode(uint8_t pin, int mode) {
  if (pin >= SIM_PIN_COUNT) {
    checkFail("gpio", "pinMode on pin %u, which does not exist", pin);
    return;
  }
  if (pin == SIM_PIN_DHT) {
    bool releasing = (mode == INPUT_PULLUP) && hal.dhtStartLowSeen;
    world->pins[pin].mode = mode;
    if (releasing) {
      uint64_t startLowUs = world->nowUs - hal.dhtStartLowUs;
      if (startLowUs < DHT_MIN_START_LOW_US) {
        checkFail("dht22", "start signal held low %.0f us, the DHT22 needs at least 1000 us", (double)startLowUs);
      }
      hal.dhtStartLowSeen = false;
      beginDhtReply();
    }
    if (mode == OUTPUT) {
      hal.dhtReplying = false;
    }
    return;
  }
  if (isRelayPin(pin)) {
    char action[32];
    snprintf(action, sizeof(action), "mode %s", modeName(mode));
    changeRelayPin(pin, mode, world->pins[pin].level, action);
    return;
  }
  world->pins[pin].mode = mode;
}

void digitalWrite(uint8_t pin, int level) {
  if (pin >= SIM_PIN_COUNT) {
    checkFail("gpio", "digitalWrite on pin %u, which does not exist", pin);
    return;
  }
  if (pin == SIM_PIN_DHT) {
    world->pins[pin].level = level;
    bool drivingLow = (world->pins[pin].mode == OUTPUT) && (level == LOW);
    if (drivingLow) {
      hal.dhtStartLowSeen = true;
      hal.dhtStartLowUs = world->nowUs;
    }
    return;
  }
  if (isRelayPin(pin)) {
    const char *action = (level == HIGH) ? "<- HIGH" : "<- LOW";
    if (world->pins[pin].mode != OUTPUT) {
      checkFail("gpio", "GP%u written while not an output", pin);
    }
    changeRelayPin(pin, world->pins[pin].mode, level, action);
    return;
  }
  world->pins[pin].level = level;
}

int digitalRead(uint8_t pin) {
  if (pin == SIM_PIN_DHT) {
    return dhtLineLevel();
  }
  if (pin >= SIM_PIN_COUNT) {
    checkFail("gpio", "digitalRead on pin %u, which does not exist", pin);
    return LOW;
  }
  return world->pins[pin].level;
}

void noInterrupts() {
  if (hal.interruptsOff) {
    return;
  }
  hal.interruptsOff = true;
  hal.interruptsOffSinceUs = world->nowUs;
}

void interrupts() {
  if (!hal.interruptsOff) {
    return;
  }
  hal.interruptsOff = false;
  uint64_t offUs = world->nowUs - hal.interruptsOffSinceUs;
  checkInterruptsOff(offUs);
}

bool simInterruptsEnabled() {
  return !hal.interruptsOff;
}

// Copies the relay writes since the last clear (pin and resulting contact, HIGH = closed); unit tests.
uint32_t simRecentRelayWrites(uint8_t *pins, int *levels, uint32_t capacity) {
  uint32_t count = hal.recentWriteCount;
  if (count > capacity) {
    count = capacity;
  }
  for (uint32_t index = 0; index < count; index++) {
    pins[index] = hal.recentWritePins[index];
    levels[index] = hal.recentWriteLevels[index];
  }
  return count;
}

void simClearRelayWrites() {
  hal.recentWriteCount = 0;
}

// ==== core mutex ====

void recursive_mutex_init(recursive_mutex_t *mutex) {
  mutex->initialised = true;
  mutex->depth = 0;
  hal.knownMutex = mutex;
}

void recursive_mutex_enter_blocking(recursive_mutex_t *mutex) {
  if (!mutex->initialised) {
    checkFail("mutex", "stateMutex taken before recursive_mutex_init()");
  }
  mutex->depth = mutex->depth + 1;
}

void recursive_mutex_exit(recursive_mutex_t *mutex) {
  if (mutex->depth <= 0) {
    checkFail("mutex", "stateMutex released more often than taken");
    return;
  }
  mutex->depth = mutex->depth - 1;
}

// Returns how deep stateMutex is held (0 = free).
int simMutexDepth() {
  if (hal.knownMutex == nullptr) {
    return 0;
  }
  return hal.knownMutex->depth;
}

// ==== EEPROM (flash) ====

EEPROMClass EEPROM;
extern "C" uint8_t _EEPROM_start[sizeof(World::flash)]; // what the firmware reads back: a copy of the flash
uint8_t _EEPROM_start[sizeof(World::flash)];

void EEPROMClass::begin(size_t size) {
  if (size > sizeof(world->flash)) {
    checkFail("eeprom", "EEPROM.begin(%u) is larger than the simulated flash sector", (unsigned)size);
    size = sizeof(world->flash);
  }
  usedSize = size;
  memcpy(ramCopy, world->flash, size);
  memcpy(_EEPROM_start, world->flash, sizeof(_EEPROM_start));
}

uint8_t EEPROMClass::read(int address) {
  return ramCopy[address];
}

void EEPROMClass::write(int address, uint8_t value) {
  ramCopy[address] = value;
}

// Writes the RAM copy to flash, or leaves the flash untouched while a scenario injects flash
// errors. Either way it reports success, as the real library does: only reading the flash back
// shows that a write failed.
bool EEPROMClass::commit() {
  if (world->failCommitsRemaining > 0) {
    world->failCommitsRemaining = world->failCommitsRemaining - 1;
    tracef("EEPROM", "commit FAILED (injected flash error), flash unchanged");
    memcpy(_EEPROM_start, world->flash, sizeof(_EEPROM_start));
    return true;
  }
  memcpy(world->flash, ramCopy, usedSize);
  memcpy(_EEPROM_start, world->flash, sizeof(_EEPROM_start));
  world->day.eepromCommits = world->day.eepromCommits + 1;
  world->total.eepromCommits = world->total.eepromCommits + 1;
  Settings stored;
  memcpy(&stored, world->flash, sizeof(stored));
  tracef("EEPROM", "commit: flash now holds mode %u, fan %u, setpoint %u F (magic %04X, firmware %08lX)",
         stored.hvacMode, stored.fanMode, stored.setpointF, stored.magic, (unsigned long)stored.firmwareId);
  checkEepromCommit();
  return true;
}

// ==== Wi-Fi ====

WiFiClass WiFi;
cyw43_t cyw43_state;

IPAddress::IPAddress(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth) {
  octets[0] = first;
  octets[1] = second;
  octets[2] = third;
  octets[3] = fourth;
}

String IPAddress::toString() const {
  char text[16];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
  return String(text);
}

void WiFiClass::mode(WiFiMode_t wifiMode) {
  (void)wifiMode;
}

void WiFiClass::setHostname(const char *name) {
  tracef("WIFI", "hostname %s", name);
}

void WiFiClass::setTimeout(unsigned long timeoutMs) {
  hal.wifiTimeoutMs = timeoutMs;
}

// Starts a join. With the access point up the join holds core 0 about 1.2 s and the address
// arrives 2.5 s after the start; without it the join holds core 0 for the whole timeout and fails.
int WiFiClass::beginNoBlock(const char *networkName, const char *password) {
  (void)password;
  world->wifi.beginCalls = world->wifi.beginCalls + 1;
  uint64_t blockedUs = 0;
  if (world->wifi.accessPointUp) {
    world->wifi.joined = true;
    world->wifi.connectedAtUs = world->nowUs + WIFI_ADDRESS_US;
    blockedUs = WIFI_JOIN_US;
    tracef("WIFI", "join %s (attempt %u): access point found, address in 2.5 s", networkName, world->wifi.beginCalls);
  } else {
    world->wifi.joined = false;
    blockedUs = hal.wifiTimeoutMs * US_PER_MS_SIM;
    tracef("WIFI", "join %s (attempt %u): no access point, gave up after %lu ms (core 0 blocked, core 1 runs on)",
           networkName, world->wifi.beginCalls, (unsigned long)hal.wifiTimeoutMs);
  }
  uint64_t busyUntilUs = world->nowUs + blockedUs;
  if (busyUntilUs > hal.core0BusyUntilUs) {
    hal.core0BusyUntilUs = busyUntilUs;
  }
  return world->wifi.joined ? 1 : 0;
}

bool simWiFiConnected() {
  bool addressAssigned = (world->nowUs >= world->wifi.connectedAtUs);
  bool connected = world->wifi.accessPointUp && world->wifi.joined && addressAssigned;
  return connected;
}

// Switches the access point on or off; switching it off drops the connection.
void simSetAccessPoint(bool up) {
  world->wifi.accessPointUp = up;
  if (!up) {
    world->wifi.joined = false;
  }
  tracef("EVENT", "Wi-Fi access point %s", up ? "back up" : "DOWN");
}

wl_status_t WiFiClass::status() {
  if (simWiFiConnected()) {
    return WL_CONNECTED;
  }
  return WL_DISCONNECTED;
}

IPAddress WiFiClass::localIP() {
  return IPAddress(192, 168, 1, 40);
}

int32_t WiFiClass::RSSI() {
  return -61;
}

void WiFiClass::disconnect() {
  world->wifi.joined = false;
}

int cyw43_wifi_pm(cyw43_t *state, uint32_t powerManagement) {
  state->powerManagement = powerManagement;
  return 0;
}

// ==== watchdog ====

watchdog_hw_t *watchdog_hw = nullptr; // pointed at world->watchdogRegisters by the runner

void watchdog_enable(uint32_t delayMs, bool pauseOnDebug) {
  (void)pauseOnDebug;
  world->watchdogEnabled = true;
  world->watchdogTimeoutMs = delayMs;
  world->watchdogLastFeedUs = simUptimeUs();
  tracef("WDOG", "watchdog enabled, %lu ms", (unsigned long)delayMs);
}

void watchdog_update() {
  world->watchdogLastFeedUs = simUptimeUs();
}

// The firmware's own restart: the Pico resets as soon as the delay runs out.
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delayMs) {
  (void)pc;
  (void)sp;
  tracef("WDOG", "watchdog_reboot(%lu ms) called by the firmware, scratch[0] = %08lX", (unsigned long)delayMs,
         (unsigned long)world->watchdogRegisters.scratch[0]);
  SimReset reset;
  reset.kind = RESET_WATCHDOG_REBOOT;
  throw reset;
}

bool watchdog_enable_caused_reboot() {
  return world->watchdogCausedReboot;
}

// The simulation has no display; display.cpp and its Adafruit library are not built here. Only
// the backlight's default level and a count of its wakes are kept, for the API routes.
static uint8_t simDisplayBrightness = DISPLAY_BRIGHTNESS_DEFAULT;
uint32_t simDisplayWakes = 0;

void startDisplay() {
  simDisplayBrightness = DISPLAY_BRIGHTNESS_DEFAULT;
}

void updateDisplay() {
}

void wakeDisplay() {
  simDisplayWakes = simDisplayWakes + 1;
}

void setDisplayDefaultBrightness(uint8_t level) {
  simDisplayBrightness = level;
}

uint8_t displayDefaultBrightness() {
  return simDisplayBrightness;
}

// ==== HTTP replies (the parts of http.cpp the API routes write into) ====

static char commandReplyOut[SIM_COMMAND_REPLY_LENGTH + 1];
static char httpBodyBuffer[SIM_HTTP_BODY_BUFFER_SIZE];
static uint16_t httpBodyLength = 0;
static bool httpReplyUsesBody = false;
static HttpCommandHandler commandHandler = nullptr;

void startHttpServer(HttpCommandHandler handler) {
  commandHandler = handler;
  tracef("HTTP", "API server started on port %u", HTTP_SERVER_PORT);
}

// Requests reach the routes through simRunCommand() instead.
void serviceHttpClient() {
}

void startBodyReply() {
  httpBodyLength = 0;
  httpBodyBuffer[0] = '\0';
  httpReplyUsesBody = true;
}

static void appendBodyFormatted(const char *format, va_list arguments) {
  uint16_t lastUsableIndex = SIM_HTTP_BODY_BUFFER_SIZE - 1;
  if (httpBodyLength >= lastUsableIndex) {
    return;
  }
  char *writePosition = httpBodyBuffer + httpBodyLength;
  size_t spaceRemaining = SIM_HTTP_BODY_BUFFER_SIZE - httpBodyLength;
  int charactersWritten = vsnprintf(writePosition, spaceRemaining, format, arguments);
  if (charactersWritten > 0) {
    uint32_t extendedLength = (uint32_t)httpBodyLength + (uint32_t)charactersWritten;
    if (extendedLength > lastUsableIndex) {
      extendedLength = lastUsableIndex;
    }
    httpBodyLength = (uint16_t)extendedLength;
  }
}

void appendBody(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

void appendBodyField(const char *label, const char *format, ...) {
  appendBody("%-21s", label);
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

void replyWithNumber(int16_t value) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%d", value);
}

void replyWithText(const char *text) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%s", text);
}

void replyWithFormat(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(commandReplyOut, sizeof(commandReplyOut), format, arguments);
  va_end(arguments);
}

// Runs one decoded request through the firmware's API routes and returns the reply as the HTTP
// server would send it: the text, or "400 ERR" when the routes left the reply empty.
std::string simRunCommand(uint16_t command, uint16_t value) {
  if (commandHandler == nullptr) {
    return "NO SERVER";
  }
  commandReplyOut[0] = '\0';
  httpReplyUsesBody = false;
  commandHandler(command, value);
  if (httpReplyUsesBody) {
    return std::string(httpBodyBuffer);
  }
  if (commandReplyOut[0] == '\0') {
    return "400 ERR";
  }
  return std::string(commandReplyOut);
}
