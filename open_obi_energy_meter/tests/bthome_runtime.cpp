#include "Arduino.h"
#include "NimBLEDevice.h"
#include "Preferences.h"
#include "WebServer.h"
#include "bthome_ble.h"
#include "reader.h"
#include <assert.h>

uint32_t testMillis = 10000;
FakeSerial Serial;
FakeStorage testStorage;
bool testStorageFail = false;
NimBLEAdvertising testAdvertising;
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

static void capture(int32_t power, unsigned index = 0) {
  readers[index].power = static_cast<uint32_t>(power);
  readers[index].lastEnergyMs = millis();
  bthome_capture(readers[index]);
  bthome_service();
}

static uint32_t sequence() {
  const auto &p = testAdvertising.payload;
  assert(p.size() == 30 && p[12] == 17 && p[13] == 0x16);
  assert(p[14] == 0xD2 && p[15] == 0xFC && p[16] == 0x41);
  return p[22] | uint32_t(p[23]) << 8 | uint32_t(p[24]) << 16 | uint32_t(p[25]) << 24;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  Reader &r = readers[0];
  r.used = r.assigned = r.haveData = true;
  r.flags = 1; r.handle[0] = 1; r.handle[1] = 2; r.handle[2] = 3;
  readers[1] = r; readers[1].handle[2] = 4;
  bthome_init(&web);
  statusContains("\"enabled\":false");
  capture(100);
  assert(!testAdvertising.isAdvertising());

  if (!strcmp(argv[1], "validation")) {
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

  configure();
  statusContains("\"key_set\":true");
  capture(-850, 1);
  assert(!testAdvertising.isAdvertising());
  capture(-850);
  assert(testAdvertising.isAdvertising());
  assert(!testAdvertising.scanResponse && testAdvertising.mode == BLE_GAP_CONN_MODE_NON);
  assert(testAdvertising.minInterval == 160 && testAdvertising.maxInterval == 240);
  assert(testAdvertising.duration == 2000);
  assert(sequence() == 0);

  if (!strcmp(argv[1], "packet")) {
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
    bthome_init(&web);
    capture(1);
    assert(sequence() >= 4096 && sequence() > previous);
    previous = sequence();
    bthome_reset(); bthome_service();
    assert(!testAdvertising.isAdvertising());
    bthome_init(&web); configure(); capture(2);
    assert(sequence() >= 8192 && sequence() > previous);
    bthome_init(&web);
    testStorageFail = true; capture(3);
    assert(!testAdvertising.isAdvertising());
    statusContains("Counter persistence failed");
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
    capture(10); assert(!testAdvertising.isAdvertising());
    web.params = {{"enabled", "1"}};
    bthome_configure(); assert(web.code == 200);
    assert(!testAdvertising.isAdvertising());
    capture(10); assert(sequence() == 3);
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
