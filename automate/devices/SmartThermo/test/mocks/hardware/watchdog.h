// =====================================================================================
// test/mocks/hardware/watchdog.h
// Host stand-in for the pico-sdk watchdog. The simulator restarts the firmware when it is not fed
// in time or when watchdog_reboot() is called; the scratch registers survive the restart.
// =====================================================================================
#pragma once

#include <stdint.h>

struct watchdog_hw_t {
  volatile uint32_t scratch[8];
};
extern watchdog_hw_t *watchdog_hw;

void watchdog_enable(uint32_t delayMs, bool pauseOnDebug);
void watchdog_update();
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delayMs);
bool watchdog_enable_caused_reboot();
