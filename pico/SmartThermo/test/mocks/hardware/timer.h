// =====================================================================================
// test/mocks/hardware/timer.h
// Host stand-in for the pico-sdk timer: microseconds since boot from the simulated clock.
// =====================================================================================
#pragma once

#include <stdint.h>

uint64_t time_us_64();
uint32_t time_us_32();
void busy_wait_us_32(uint32_t delayUs);
