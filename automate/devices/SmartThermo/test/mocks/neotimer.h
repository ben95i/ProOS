// =====================================================================================
// test/mocks/neotimer.h
// Host stand-in for the Neotimer library: repeat() returns true once per interval of millis().
// =====================================================================================
#pragma once

#include <Arduino.h>

class Neotimer {
 public:
  Neotimer(uint32_t intervalMs = 1000) : interval(intervalMs) {
  }

  // true once every interval; the first call starts the timer
  bool repeat() {
    uint32_t nowMs = millis();
    if (!started) {
      started = true;
      lastMs = nowMs;
      return false;
    }
    uint32_t elapsedMs = nowMs - lastMs;
    if (elapsedMs < interval) {
      return false;
    }
    lastMs = nowMs;
    return true;
  }

 private:
  uint32_t interval;
  uint32_t lastMs = 0;
  bool started = false;
};
