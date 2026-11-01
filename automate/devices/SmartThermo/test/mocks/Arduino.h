// =====================================================================================
// test/mocks/Arduino.h
// Host stand-in for the arduino-pico core: only what the SmartThermo firmware uses. Time, GPIO and
// the DHT22 data line are simulated by sim_hal.cpp, so the firmware runs unchanged on a PC.
// =====================================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

// pin levels and modes, as in the arduino-pico core
const int LOW = 0;
const int HIGH = 1;
const int INPUT = 0;
const int OUTPUT = 1;
const int INPUT_PULLUP = 2;
const uint8_t LED_BUILTIN = 64; // the Pico W's LED sits on the Wi-Fi chip, numbered 64 by the core

// time since boot, from the simulated clock
uint32_t millis();
void delay(uint32_t milliseconds);
void delayMicroseconds(uint32_t microseconds);

// GPIO, recorded and traced by the simulator
void pinMode(uint8_t pin, int mode);
void digitalWrite(uint8_t pin, int level);
int digitalRead(uint8_t pin);
void noInterrupts();
void interrupts();

// pico-sdk busy-wait hint
inline void tight_loop_contents() {
}

// the parts of Arduino's String the firmware uses: holding text and c_str()
class String {
 public:
  String() {
  }
  String(const char *text) : value(text) {
  }
  const char *c_str() const {
    return value.c_str();
  }

 private:
  std::string value;
};
