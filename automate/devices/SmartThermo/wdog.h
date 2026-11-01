// =====================================================================================
// wdog.h
// SmartThermo: the RP2040 hardware watchdog and the restart the firmware asks for.
// Core 1 feeds the watchdog on every pass while core 0 is also running; the Pico restarts if
// core 1 stops for WATCHDOG_TIMEOUT_MS or core 0 for CORE0_STALL_LIMIT_MS. The restart resets
// every pin, which opens every relay. Watchdog restarts in a row are counted, because each one
// restarts the 5 min power-up delay: SAFE_MODE_RESTART_COUNT of them start safe mode.
// =====================================================================================
#pragma once

#include "common.h"

const uint32_t WATCHDOG_TIMEOUT_MS = 8000;   // the RP2040 maximum is about 8.3 s
const uint32_t CORE0_STALL_LIMIT_MS = 30000; // core 0 may pause this long (see the budget check in SmartThermo.ino)
const uint16_t SAFE_MODE_RESTART_COUNT = 3;  // watchdog restarts in a row that start safe mode

extern bool restartedByWatchdog; // true when the last reset came from the watchdog (a stall)
extern bool restartedForWiFi;    // true when the last reset was the Wi-Fi restart
extern uint16_t watchdogRestartsInARow; // cleared by any other reset, and once the Pico has run steadily

void readRestartCause();
void startWatchdog();
void markCore0Alive();
void feedWatchdogWhileCore0Runs();
void clearWatchdogRestartCount();
void pauseCore0StallCheck();
void resumeCore0StallCheck();
[[noreturn]] void restartWithWiFiCause();
