// =====================================================================================
// test/mocks/EEPROM.h
// Host stand-in for the arduino-pico EEPROM library: a RAM copy that begin() fills from the
// simulated flash and commit() writes back. The flash survives simulated restarts and power
// loss, and the scenarios can corrupt it or make writes fail (see sim_hal.cpp). As on the Pico,
// _EEPROM_start reads the flash itself.
// =====================================================================================
#pragma once

#include <Arduino.h>

const size_t SIM_EEPROM_MAX_SIZE = 4096;

class EEPROMClass {
 public:
  void begin(size_t size);
  uint8_t read(int address);
  void write(int address, uint8_t value);
  bool commit();

  // the RAM copy; on the Pico asking for it also marks the copy as changed
  uint8_t *getDataPtr() {
    return ramCopy;
  }

  // copies a value out of the RAM copy, e.g. EEPROM.get(0, settings)
  template <typename T> T &get(int address, T &target) {
    memcpy(&target, ramCopy + address, sizeof(T));
    return target;
  }

  // copies a value into the RAM copy; commit() writes it to flash
  template <typename T> const T &put(int address, const T &source) {
    memcpy(ramCopy + address, &source, sizeof(T));
    return source;
  }

 private:
  uint8_t ramCopy[SIM_EEPROM_MAX_SIZE];
  size_t usedSize = 0;
};
extern EEPROMClass EEPROM;
