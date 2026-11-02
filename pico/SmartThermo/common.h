// =====================================================================================
// common.h
// SmartThermo: constants, settings and helper functions shared by the sketch and its modules
// (http, thermo, wdog). Every variable and function declared here is defined in SmartThermo.ino.
// =====================================================================================
#pragma once

#include <Arduino.h>

// ---- common resources ----
const char NULL_TERMINATOR = '\0';
const char NEW_LINE = '\n';
const char CARRIAGE_RETURN = '\r';
const uint32_t US_PER_MS = 1000;
const uint32_t MS_PER_SECOND = 1000;
const uint32_t SECONDS_PER_MINUTE = 60;
const uint32_t SECONDS_PER_HOUR = 3600;
const uint32_t SECONDS_PER_DAY = 86400;
const int32_t TENTHS_PER_DEGREE = 10;     // temperatures are kept in tenths of a degree
const uint8_t TEMPERATURE_TEXT_SIZE = 24; // a reading as text, e.g. "71.3 F (last reading)"

// ---- hvac and fan modes (stored in the settings) ----
const uint16_t HVAC_MODE_OFF = 0;  // nothing heats or cools
const uint16_t HVAC_MODE_HEAT = 1; // W1 below the setpoint
const uint16_t HVAC_MODE_COOL = 2; // G + Y1 above the setpoint
const uint16_t FAN_MODE_AUTO = 0;  // G only while cooling (the furnace runs its own blower on heat)
const uint16_t FAN_MODE_ON = 1;    // G always closed

// ---- persistent settings (stored in EEPROM by SmartThermo.ino) ----
struct Settings {
  uint32_t firmwareId; // ID of the firmware build that saved this block
  uint16_t magic;
  uint8_t hvacMode;    // HVAC_MODE_OFF / HVAC_MODE_HEAT / HVAC_MODE_COOL
  uint8_t fanMode;     // FAN_MODE_AUTO / FAN_MODE_ON
  uint16_t setpointF;  // whole degrees F
  uint16_t textColor;  // place in DISPLAY_TEXT_COLORS (display.h)
  uint16_t coolShutOff; // 1 after a failed sensor switched cool off, until the hvac mode is next set
  uint16_t checksum;
};
// the checksum, the stored-copy comparison and the EEPROM image all treat Settings as raw
// bytes, so it must not contain padding bytes (their contents are undefined)
static_assert(sizeof(Settings) == 16, "Settings must not contain padding bytes");
extern Settings settings;         // live settings
extern bool settingsSavePending;  // true while a change is waiting for its EEPROM write
extern uint64_t bootMs;           // uptime when setup() started

// ---- time ----
uint64_t uptimeMs();
uint64_t msSince(uint64_t sinceMs, uint64_t nowMs);
bool intervalElapsed(uint64_t sinceMs, uint64_t intervalMs, uint64_t nowMs);
uint64_t msRemaining(uint64_t sinceMs, uint64_t intervalMs, uint64_t nowMs);
unsigned long msToSeconds(uint64_t durationMs);
unsigned long msToSecondsRoundedUp(uint64_t durationMs);

// ---- state shared between the cores (stateMutex) ----
void lockState();
void unlockState();

// ---- log, text and settings helpers ----
void logMessage(const char *format, ...);
void formatTenths(char *output, size_t outputSize, int32_t tenths);
const char *lookupName(const char *const names[], uint8_t nameCount, uint16_t index, const char *fallback);
uint16_t sumBytes(const uint8_t *bytes, size_t length);
void markSettingsChanged();
