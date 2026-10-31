#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include "bthome_ble.h"
#include "bthome_core.h"
#include "reader.h"
#include "bench_secrets.h"

const int MAX_READERS = 1;
Reader readers[MAX_READERS];
static WebServer server(80);
static bool paused = false, automatic = true;
static int32_t power = 1234;
static uint32_t lastSample = 0, lastLog = 0;
static String command;

static bool seedConfig() {
  uint8_t data[24] = {1, 1, 1, 0x01, 0x02, 0x03, 5, 0};
  if (!bthome::parseHex(BENCH_BINDKEY, data + 8, 16)) return false;
  Preferences p;
  if (!p.begin("obiblecount", false)) return false;
  bool ok = p.isKey("high") || p.putULong64("high", 0) == sizeof(uint64_t);
  p.end();
  if (!ok || !p.begin("obible", false)) return false;
  uint8_t existing[24];
  bool same = p.getBytesLength("config") == sizeof existing &&
      p.getBytes("config", existing, sizeof existing) == sizeof existing &&
      !memcmp(existing, data, sizeof data);
  ok = same || p.putBytes("config", data, sizeof data) == sizeof data;
  p.end();
  return ok;
}

static void wifiOn() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("OBI-BLE-Test", BENCH_AP_PASSWORD);
}

static void runCommand() {
  command.trim();
  if (command == "pause") paused = true;
  else if (command == "resume") paused = false;
  else if (command == "auto") { automatic = true; paused = false; }
  else if (command == "wifi off") WiFi.mode(WIFI_OFF);
  else if (command == "wifi on") wifiOn();
  else if (command == "reboot") { Serial.println("Rebooting"); Serial.flush(); delay(100); ESP.restart(); }
  else if (command.startsWith("power ")) {
    char *end = nullptr;
    const char *value = command.c_str() + 6;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end || parsed < -1000000 || parsed > 1000000) {
      Serial.println("Invalid test power"); command = ""; return;
    }
    power = parsed; automatic = false; paused = false;
  } else { Serial.println("Unknown command"); command = ""; return; }
  Serial.println("OK " + command);
  command = "";
  lastSample = millis() - 1000;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("OBI BLE bench: simulated readings");
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  Serial.printf("BLE %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Reader &r = readers[0];
  r.used = r.assigned = r.haveData = true;
  r.flags = 1; r.handle[0] = 1; r.handle[1] = 2; r.handle[2] = 3;
  if (!seedConfig()) {
    Serial.println("Bench configuration failed");
    for (;;) delay(1000);
  }
  wifiOn();
  bthome_init(&server);
  server.on("/api/bthome", HTTP_GET, bthome_status);
  server.begin();
  Serial.println("Ready: +1234 / 0 / -850 W");
}

void loop() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') runCommand();
    else if (ch != '\r' && command.length() < 80) command += ch;
  }
  uint32_t now = millis();
  if (!paused && now - lastSample >= 1000) {
    lastSample = now;
    if (automatic) {
      const int32_t values[] = {1234, 0, -850};
      power = values[(now / 10000) % 3];
    }
    readers[0].power = static_cast<uint32_t>(power);
    readers[0].import_ = 4646884 + now / 1000;   // fake meter counters (Wh)
    readers[0].export_ = 77402;
    readers[0].lastEnergyMs = now;
    bthome_capture(readers[0]);
  }
  bthome_service();
  server.handleClient();
  if (now - lastLog >= 5000) {
    lastLog = now;
    Serial.printf("BENCH ms=%lu power=%ld paused=%u heap=%u min_heap=%u wifi=%u\n",
                  (unsigned long)now, (long)power, paused, ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), unsigned(WiFi.getMode()));
  }
  delay(5);
}
