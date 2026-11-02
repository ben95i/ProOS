// =====================================================================================
// wdog.cpp
// SmartThermo: the RP2040 hardware watchdog, core 0's heartbeat and the restart cause kept in a
// watchdog scratch register. See wdog.h.
// =====================================================================================
#include <hardware/watchdog.h>

#include "wdog.h"

// A restart the firmware asks for (Wi-Fi not reconnected) leaves this mark in watchdog scratch
// register 0, which survives the restart, so the next boot can tell why it restarted.
const uint8_t RESTART_CAUSE_SCRATCH = 0;
const uint32_t RESTART_CAUSE_WIFI = 0x57F1A0B7;
const uint32_t RESTART_DELAY_MS = 10; // watchdog_reboot() delay
// Watchdog restarts in a row live in scratch register 1, which survives them and is cleared by a
// power cut. The upper half marks the lower half as a count.
const uint8_t RESTART_COUNT_SCRATCH = 1;
const uint32_t RESTART_COUNT_MARK = 0xC8A50000;
const uint32_t RESTART_COUNT_MARK_MASK = 0xFFFF0000;
const uint32_t RESTART_COUNT_VALUE_MASK = 0x0000FFFF;
volatile uint32_t core0HeartbeatMs = 0; // millis() of core 0's latest pass
volatile bool core0StallCheckPaused = false; // true while core 0 only runs the display (safe mode)
bool restartedByWatchdog = false;
bool restartedForWiFi = false;
uint16_t watchdogRestartsInARow = 0;

// Stores the watchdog restart count for the next boot and keeps it in watchdogRestartsInARow.
void storeWatchdogRestartCount(uint16_t restartCount) {
  watchdogRestartsInARow = restartCount;
  uint32_t markedCount = RESTART_COUNT_MARK | restartCount;
  watchdog_hw->scratch[RESTART_COUNT_SCRATCH] = markedCount;
}

// Returns the watchdog restart count the last boot stored, or 0 when the register holds none.
uint16_t storedWatchdogRestartCount() {
  uint32_t markedCount = watchdog_hw->scratch[RESTART_COUNT_SCRATCH];
  uint32_t mark = markedCount & RESTART_COUNT_MARK_MASK;
  if (mark != RESTART_COUNT_MARK) {
    return 0;
  }
  uint16_t restartCount = (uint16_t)(markedCount & RESTART_COUNT_VALUE_MASK);
  return restartCount;
}

// Reads and clears why the Pico last restarted: the watchdog after a stall (restartedByWatchdog),
// the Wi-Fi restart (restartedForWiFi), or neither (power-up, reset button, upload), and counts a
// watchdog restart on top of the ones before it. Call once from setup(), before startWatchdog().
void readRestartCause() {
  restartedByWatchdog = watchdog_enable_caused_reboot();
  uint32_t restartCause = watchdog_hw->scratch[RESTART_CAUSE_SCRATCH];
  restartedForWiFi = (restartCause == RESTART_CAUSE_WIFI);
  watchdog_hw->scratch[RESTART_CAUSE_SCRATCH] = 0;
  uint16_t restartCount = 0;
  if (restartedByWatchdog) {
    uint16_t previousCount = storedWatchdogRestartCount();
    restartCount = previousCount;
    if (previousCount < RESTART_COUNT_VALUE_MASK) {
      restartCount = previousCount + 1;
    }
  }
  storeWatchdogRestartCount(restartCount);
}

// Forgets the watchdog restarts in a row; called once the Pico has run steadily.
void clearWatchdogRestartCount() {
  storeWatchdogRestartCount(0);
}

// Stops core 0 stalling from restarting the Pico. Safe mode uses this so a fault in what core 0
// still runs (the display) can't keep restarting the 5 min power-up delay and starve the HVAC.
void pauseCore0StallCheck() {
  core0StallCheckPaused = true;
}

// Starts checking core 0 again, with a fresh heartbeat, before it starts networking.
void resumeCore0StallCheck() {
  markCore0Alive();
  core0StallCheckPaused = false;
}

// Stamps core 0's heartbeat; called on every pass of core 0's loop.
void markCore0Alive() {
  core0HeartbeatMs = millis();
}

// Starts the watchdog with a fresh core 0 heartbeat; called once from setup(), before core 1
// starts feeding it. pause_on_debug: a debugger halting the cores doesn't trigger a restart.
void startWatchdog() {
  markCore0Alive();
  watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}

// Feeds the watchdog while core 0 is also running: core 0 stamps core0HeartbeatMs on every pass,
// and a stamp older than CORE0_STALL_LIMIT_MS means core 0 has stalled, so the watchdog is left
// to restart the Pico. A stalled core 1 stops feeding it too.
void feedWatchdogWhileCore0Runs() {
  // read the stamp before millis(), so its age can never come out negative
  uint32_t heartbeatMs = core0HeartbeatMs;
  uint32_t currentMs = millis();
  uint32_t heartbeatAgeMs = currentMs - heartbeatMs;
  bool core0Running = (heartbeatAgeMs < CORE0_STALL_LIMIT_MS);
  if (core0Running || core0StallCheckPaused) {
    watchdog_update();
  }
}

// Restarts the Pico through the watchdog, leaving RESTART_CAUSE_WIFI in the scratch register for
// the next boot (see readRestartCause). Never returns. The caller opens the relays first.
void restartWithWiFiCause() {
  watchdog_hw->scratch[RESTART_CAUSE_SCRATCH] = RESTART_CAUSE_WIFI;
  watchdog_reboot(0, 0, RESTART_DELAY_MS);
  while (true) {
    tight_loop_contents(); // the watchdog resets the Pico within RESTART_DELAY_MS
  }
}
