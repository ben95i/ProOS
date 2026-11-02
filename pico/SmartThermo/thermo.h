// =====================================================================================
// thermo.h
// SmartThermo: the DHT22 sensor, the thermostat and the W1 / Y1 / G relays with every interlock
// and lockout from the HVAC Thermostat Control Reference. Runs on core 1 (readSensorWhenDue,
// updateThermostat); everything else is read by the API routes on core 0. Call every function
// with stateMutex held, except during setup() before core 1 starts.
// =====================================================================================
#pragma once

#include "common.h"

// ---- GPIO pins ----
// Each relay coil is driven active high through a 2N5551 NPN transistor (never straight from
// the pin), so HIGH closes the contact. The pins are set to 12 mA drive strength for its base. The RP2040 holds every GPIO as an input with its
// pull-down from reset until setup() runs, so a relay driven this way stays open through
// power-up, a watchdog restart and a firmware upload. None of these pins is used by the Wi-Fi
// chip (GP23-25, GP29), the ADC (GP26-28) or the default debug UART (GP0-1).
const uint8_t RELAY_FAN_G_PIN = 16;   // GP16 = physical pin 21, contact R -> G (indoor blower)
const uint8_t RELAY_COOL_Y1_PIN = 17; // GP17 = physical pin 22, contact R -> Y1 (compressor contactor)
const uint8_t RELAY_HEAT_W1_PIN = 18; // GP18 = physical pin 24, contact R -> W1 (furnace heat call)
const uint8_t DHT22_DATA_PIN = 22;    // GP22 = physical pin 29, DHT22 data with a 10k pull-up to 3V3

// heat and cool can't start for this long after boot: power-up counts as both just stopping
const uint64_t POWER_UP_DELAY_MS = 300000; // 5 min

// ---- sensor states (reported by ?2,0,2~) ----
const int16_t SENSOR_STATE_OK = 0;       // fresh readings, the thermostat follows them
const int16_t SENSOR_STATE_STARTING = 1; // no reading yet, under DHT_STARTUP_TIMEOUT_MS since boot; heat and cool wait
const int16_t SENSOR_STATE_LOCKOUT = 2;  // no reading since boot: HVAC locked out, every relay open, until the sensor reads (heat mode: until failed)
const int16_t SENSOR_STATE_STALE = 3;    // no valid reading for DHT_STALE_MS: no new starts, running cycles stop after their minimum
const int16_t SENSOR_STATE_FAILED = 4;   // no valid reading for DHT_FAILED_MS, also from boot in heat mode: heat runs 25%, cool is switched off

// ---- running states (reported by ?2,0,4~) ----
const int16_t RUNNING_IDLE = 0;
const int16_t RUNNING_HEATING = 1;
const int16_t RUNNING_COOLING = 2;
const int16_t RUNNING_FAN_ONLY = 3;

struct RelayOutputs {
  bool heatW1; // true = R closed to W1
  bool coolY1; // true = R closed to Y1
  bool fanG;   // true = R closed to G
};

// ---- start-up (core 0, in setup()) ----
void initRelays();     // opens every relay; the first thing setup() does
void initThermostat(); // starts the HVAC timers and schedules the sensor's first reading

// ---- HVAC control (core 1) ----
void readSensorWhenDue();
void updateThermostat();

// ---- state for the API routes and the Wi-Fi restart ----
void openAllRelays();
void clearRunLimits(); // a person set the hvac mode
bool hvacCycleRunning(); // W1 or Y1 closed, or the blower run-on after cooling
RelayOutputs currentRelays();
int16_t currentSensorState();
int16_t runningState();
void formatTemperatureReading(char *output, size_t outputSize);
void formatHumidityReading(char *output, size_t outputSize);
bool readWholeTemperatureF(int16_t &wholeDegreesF); // false while there is no usable reading
uint32_t sensorSampleCount();                       // DHT22 samples taken since boot, valid or not

// ---- status reply sections ----
void appendSensorStatus(uint64_t nowMs);
void appendSetpointStatus();
void appendEquipmentStatus(uint64_t nowMs);
bool sensorHasError();
void appendSensorErrors(uint64_t nowMs);
bool interlocksHaveOverridden();
void appendInterlockErrors();
bool runLimitsHaveError();
void appendRunLimitErrors(uint64_t nowMs);
