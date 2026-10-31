#include "Arduino.h"
#include "NimBLEDevice.h"
#include "esp_bt.h"
#include "Preferences.h"
#include "WebServer.h"
#include "bthome_ble.h"
#include "reader.h"
#include <assert.h>
#include <stdlib.h>

uint32_t testMillis = 10000;
FakeSerial Serial;
FakeStorage testStorage;
bool testStorageFail = false;
NimBLEAdvertising testAdvertising;
unsigned testBleInitCalls = 0;
unsigned testBleDeinitCalls = 0;
unsigned testBtReleases = 0;
const int MAX_READERS = 2;
Reader readers[MAX_READERS];
WebServer web;
static const char *key = "231d39c1d7cc1ab1aee224cd096db932";

static void statusContains(const char *text) {
  bthome_status();
  assert(web.code == 200 && web.body.find(text) != std::string::npos);
  assert(web.body.find(key) == std::string::npos);
}

static void configure() {
  web.params = {{"enabled", "1"}, {"reader", "010203"}, {"key", key}, {"stale_seconds", "5"}};
  bthome_configure();
  assert(web.code == 200);
}

// The 24-byte blob bthome_configure() persists: version 1, enabled, key present, reader 01:02:03,
// stale 5 s, bindkey. Writing it before bthome_init() puts the module in the state a real device is in
// after setup AND a reboot -- the only state the radio can still start from, since a boot that finds
// BTHome disabled hands its RAM to the general heap.
static void seedEnabled() {
  std::vector<uint8_t> blob(24, 0);
  blob[0] = 1; blob[1] = 1; blob[2] = 1;
  blob[3] = 1; blob[4] = 2; blob[5] = 3;
  blob[6] = 5;
  for (int i = 0; i < 16; ++i) {
    const std::string byte(key + 2 * i, 2);
    blob[8 + i] = (uint8_t)strtoul(byte.c_str(), nullptr, 16);
  }
  testStorage["obible"]["config"] = blob;
  testStorage["obiblecount"]["high"] = std::vector<uint8_t>(8, 0);
}

static void capture(int32_t power, unsigned index = 0) {
  readers[index].power = static_cast<uint32_t>(power);
  readers[index].lastEnergyMs = millis();
  bthome_capture(readers[index]);
  bthome_service();
}

static uint32_t sequence() {
  const auto &p = testAdvertising.payload;
  const size_t ad = p.size() == 31 ? 27 : 17;   // with or without import/export
  assert(p.size() == 4 + ad && p[3] == ad && p[4] == 0x16);
  assert(p[5] == 0xD2 && p[6] == 0xFC && p[7] == 0x41);
  const size_t c = 3 + ad - 7;
  return p[c] | uint32_t(p[c + 1]) << 8 | uint32_t(p[c + 2]) << 16 | uint32_t(p[c + 3]) << 24;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  Reader &r = readers[0];
  r.used = r.assigned = r.haveData = true;
  r.flags = 1; r.handle[0] = 1; r.handle[1] = 2; r.handle[2] = 3;
  r.import_ = 4646884; r.export_ = 77402;
  readers[1] = r; readers[1].handle[2] = 4;

  const bool bootsEnabled = strcmp(argv[1], "validation") != 0 &&
                            strcmp(argv[1], "disabled_boot") != 0;
  if (bootsEnabled) seedEnabled();
  bthome_init(&web);

  // A disabled boot must give the radio's RAM away and must not be able to take it back without one.
  if (!strcmp(argv[1], "disabled_boot")) {
    statusContains("\"enabled\":false");
    assert(testBtReleases == 1 && testBleInitCalls == 0);
    capture(100);
    assert(!testAdvertising.isAdvertising());
    configure();                                   // stored, but cannot take effect until a reboot
    statusContains("\"reboot_required\":true");
    statusContains("Reboot required");
    capture(100);
    assert(!testAdvertising.isAdvertising() && testBleInitCalls == 0);
    return 0;
  }

  if (!strcmp(argv[1], "validation")) {
    statusContains("\"enabled\":false");
    capture(100);
    assert(!testAdvertising.isAdvertising());
    assert(testBleInitCalls == 0);
    web.params = {{"enabled", "1"}, {"reader", "010203"}};
    bthome_configure(); assert(web.code == 400);
    for (const auto &entry : std::vector<std::pair<String, String>>{
        {"reader", "112233"}, {"key", "abcd"}, {"key", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"},
        {"stale_seconds", "0"}, {"stale_seconds", "4"}, {"stale_seconds", "3601"},
        {"stale_seconds", "5x"}, {"enabled", "true"}, {"typo", "1"}}) {
      web.params = {{"enabled", "1"}, {"reader", "010203"}, {"key", key}};
      bool found = false;
      for (auto &p : web.params) if (p.first == entry.first) { p.second = entry.second; found = true; }
      if (!found) web.params.push_back(entry);
      bthome_configure(); assert(web.code == 400);
      assert(!testAdvertising.isAdvertising());
    }
    r.devType = 0x11;
    web.params = {{"enabled", "1"}, {"reader", "010203"}, {"key", key}};
    bthome_configure(); assert(web.code == 400);
    return 0;
  }

  // Booted with BTHome already enabled: the radio's RAM is still ours and nothing has touched it yet.
  assert(testBtReleases == 0 && testBleInitCalls == 0);
  statusContains("\"key_set\":true");
  statusContains("\"reboot_required\":false");
  capture(-850, 1);
  assert(!testAdvertising.isAdvertising());
  capture(-850);
  assert(testAdvertising.isAdvertising());
  assert(testBleInitCalls == 1);
  assert(testAdvertising.scanResponse && testAdvertising.mode == BLE_GAP_CONN_MODE_NON);
  assert(testAdvertising.scanPayload == std::vector<uint8_t>({8, 9, 'O', 'B', 'I', '-', 'P', 'W', 'R'}));
  assert(testAdvertising.minInterval == 160 && testAdvertising.maxInterval == 240);
  assert(testAdvertising.duration == 2000);
  assert(sequence() == 0);

  if (!strcmp(argv[1], "energy_guard")) {
    assert(testAdvertising.payload.size() == 21);              // first frame: power only
    auto step = [&](uint32_t imp, uint32_t exp) {
      testMillis += 3000; r.import_ = imp; r.export_ = exp; capture(-850);
      return testAdvertising.payload.size();
    };
    assert(step(4646890, 77402) == 31);                        // two frames agree
    assert(step(0, 77402) == 21);                              // zeroed counter
    assert(step(4646891, 77402) == 21);                        // implausible vs. the 0
    assert(step(4646892, 77402) == 31);
    assert(step(4646880, 77402) == 21);                        // falling: dip never sent
    assert(step(4646893, 77402) == 31);
    assert(step(4600000, 77402) == 21);                        // implausible drop
    assert(step(4646894, 77402) == 21);                        // implausible vs. the drop
    assert(step(4646895, 77402) == 31);
    assert(step(0x7FFFFFFF, 77402) == 21);                     // n/a
    return 0;
  }
  if (!strcmp(argv[1], "packet")) {
    testMillis += 3000; capture(-850);                         // second frame confirms the counters
    for (auto b : testAdvertising.payload) printf("%02x", b);
    puts(""); return 0;
  }
  if (!strcmp(argv[1], "lifecycle")) {
    testMillis += 20; capture(1234);
    assert(testAdvertising.starts == 1);
    testMillis += 80; bthome_service();
    assert(testAdvertising.starts == 2 && sequence() == 1);
    testMillis += 1000; bthome_service();
    assert(sequence() == 2);
    testMillis += 2000;
    assert(!testAdvertising.isAdvertising());
    bthome_service();
    assert(testAdvertising.isAdvertising() && testAdvertising.duration == 1920);
    testMillis = readers[0].lastEnergyMs + 5000;
    bthome_service(); assert(!testAdvertising.isAdvertising());
    capture(0); assert(testAdvertising.isAdvertising());
    testMillis += 100; capture(0x7FFFFFFF);
    assert(!testAdvertising.isAdvertising());
    testMillis += 100; capture(-100);
    assert(testAdvertising.isAdvertising());
    r.flags = 0; capture(100);
    assert(!testAdvertising.isAdvertising());
    r.flags = 1; capture(100);
    assert(testAdvertising.isAdvertising());
    bthome_forget(r.handle); bthome_service();
    assert(!testAdvertising.isAdvertising());
    return 0;
  }
  if (!strcmp(argv[1], "storage")) {
    uint32_t previous = sequence();
    testStorageFail = true;
    web.params = {{"enabled", "0"}};
    bthome_configure(); assert(web.code == 503);
    statusContains("\"enabled\":true");
    testStorageFail = false;
    bthome_init(&web);                 // a reboot that still finds BTHome on: the radio keeps its RAM
    capture(1);
    assert(sequence() >= 4096 && sequence() > previous);   // resumes at the next reserved block
    bthome_init(&web);
    testStorageFail = true; capture(2);
    assert(!testAdvertising.isAdvertising());
    statusContains("Counter persistence failed");
    return 0;
  }
  // A factory reset drops the configuration, keeps the counter and hands the radio's RAM back, so like
  // any other switch-off it can only start again after a reboot. That the counter then carries on at the
  // next reserved block is what the "storage" scenario above checks.
  if (!strcmp(argv[1], "factory_reset")) {
    assert(testAdvertising.isAdvertising());
    bthome_reset(); bthome_service();
    assert(!testAdvertising.isAdvertising());
    assert(testStorage["obible"].empty());
    assert(testStorage["obiblecount"].count("high") == 1);
    assert(testBleDeinitCalls == 1 && testBtReleases == 1);
    bthome_init(&web); configure();
    statusContains("\"reboot_required\":true");
    capture(1);
    assert(!testAdvertising.isAdvertising());
    return 0;
  }
  if (!strcmp(argv[1], "key_rotation")) {
    web.params = {{"key", "00112233445566778899aabbccddeeff"}};
    bthome_configure(); assert(web.code == 200);
    capture(25); assert(sequence() == 1);
    web.params = {{"key", key}};
    bthome_configure(); assert(web.code == 200);
    capture(-25); assert(sequence() == 2);
    web.params = {{"enabled", "0"}};
    bthome_configure(); assert(web.code == 200);
    assert(testBleDeinitCalls == 1 && testBtReleases == 1);   // heap handed back immediately
    capture(10); assert(!testAdvertising.isAdvertising());
    web.params = {{"enabled", "1"}};
    bthome_configure(); assert(web.code == 200);
    statusContains("\"reboot_required\":true");
    capture(10);
    assert(!testAdvertising.isAdvertising() && testBleInitCalls == 1);
    return 0;
  }
  if (!strcmp(argv[1], "missing_counter")) {
    testStorage["obiblecount"].clear();
    bthome_init(&web); capture(5);
    assert(!testAdvertising.isAdvertising());
    statusContains("Counter unavailable");
    return 0;
  }
  if (!strcmp(argv[1], "advertising_error")) {
    testAdvertising.failStart = true;
    testMillis += 1000; bthome_service();
    assert(!testAdvertising.isAdvertising());
    statusContains("Bluetooth advertising failed");
    return 0;
  }
  return 2;
}
