// =====================================================================================
// test/mocks/WiFi.h
// Host stand-in for the arduino-pico WiFi library: a simulated access point that the scenarios
// switch on and off (see sim_hal.cpp). Only the calls SmartThermo.ino makes are provided.
// =====================================================================================
#pragma once

#include <Arduino.h>

enum wl_status_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6
};

enum WiFiMode_t {
  WIFI_OFF = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3
};

class IPAddress {
 public:
  IPAddress(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth);
  String toString() const;

 private:
  uint8_t octets[4];
};

class WiFiClass {
 public:
  void mode(WiFiMode_t wifiMode);
  void setHostname(const char *name);
  void setTimeout(unsigned long timeoutMs);
  int beginNoBlock(const char *networkName, const char *password);
  wl_status_t status();
  IPAddress localIP();
  int32_t RSSI();
  void disconnect();
};
extern WiFiClass WiFi;

// CYW43 power management (the firmware turns power saving off)
struct cyw43_t {
  uint32_t powerManagement;
};
extern cyw43_t cyw43_state;
const uint32_t CYW43_DEFAULT_PM = 0xA11142;
int cyw43_wifi_pm(cyw43_t *state, uint32_t powerManagement);
