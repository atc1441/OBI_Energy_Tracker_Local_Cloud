#pragma once
#include "Arduino.h"
#include <vector>

#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_GAP_CONN_MODE_NON 0

class NimBLEUUID {
 public:
  uint16_t value;
  explicit NimBLEUUID(uint16_t v) : value(v) {}
};

class NimBLEAdvertisementData {
 public:
  std::vector<uint8_t> payload;
  bool setFlags(uint8_t flags) { payload.insert(payload.end(), {2, 1, flags}); return true; }
  bool setName(const char *name) {
    payload.push_back(strlen(name) + 1); payload.push_back(9);
    payload.insert(payload.end(), name, name + strlen(name));
    return payload.size() <= 31;
  }
  bool setServiceData(NimBLEUUID uuid, const uint8_t *data, size_t size) {
    payload.push_back(size + 3); payload.push_back(0x16);
    payload.push_back(uuid.value & 0xFF); payload.push_back(uuid.value >> 8);
    payload.insert(payload.end(), data, data + size);
    return payload.size() <= 31;
  }
};

class NimBLEAdvertising {
 public:
  std::vector<uint8_t> payload, scanPayload;
  bool active = false, failStart = false, scanResponse = false;
  uint32_t started = 0, duration = 0;
  unsigned starts = 0;
  uint16_t minInterval = 0, maxInterval = 0;
  int mode = -1;
  void enableScanResponse(bool on) { scanResponse = on; }
  void setMinInterval(uint16_t value) { minInterval = value; }
  void setMaxInterval(uint16_t value) { maxInterval = value; }
  bool setConnectableMode(int value) { mode = value; return true; }
  bool stop() { active = false; return true; }
  bool setAdvertisementData(const NimBLEAdvertisementData &data) { payload = data.payload; return true; }
  bool setScanResponseData(const NimBLEAdvertisementData &data) { scanPayload = data.payload; return true; }
  bool start(uint32_t ms) {
    if (failStart) return false;
    duration = ms; started = millis(); active = true; ++starts; return true;
  }
  bool isAdvertising() { return active && (duration == 0 || uint32_t(millis() - started) < duration); }
};

extern NimBLEAdvertising testAdvertising;
extern unsigned testBleInitCalls;
extern unsigned testBleDeinitCalls;
class NimBLEDevice {
 public:
  static bool init(const char *) { ++testBleInitCalls; return true; }
  static bool deinit(bool) { ++testBleDeinitCalls; return true; }
  static bool setOwnAddrType(int value) { return value == BLE_OWN_ADDR_PUBLIC; }
  static NimBLEAdvertising *getAdvertising() { return &testAdvertising; }
};
