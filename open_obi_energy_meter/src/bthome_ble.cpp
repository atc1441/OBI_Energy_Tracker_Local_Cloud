#ifdef OBI_BTHOME

#include "bthome_core.h"
#include "bthome_ble.h"
#include "reader.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_bt.h>
#include <esp_mac.h>
#include <mbedtls/ccm.h>

namespace {

struct Config {
  bool enabled = false;
  bool hasKey = false;
  uint8_t reader[3] = {};
  uint8_t key[16] = {};
  uint16_t stale = 60;
};

struct Sample {
  bool valid = false;
  uint32_t power = 0;
  uint32_t received = 0;
  uint32_t generation = 0;
};

Config config;
Sample sample;
portMUX_TYPE sampleMux = portMUX_INITIALIZER_UNLOCKED;
WebServer *server = nullptr;
NimBLEAdvertising *advertising = nullptr;
bthome::Counter counter;
uint8_t mac[6] = {};
uint32_t lastSend = 0, lastGeneration = 0, frames = 0;
bool haveSent = false, counterReady = false;
bool bleReleased = false;          // radio RAM handed to the general heap -- BLE needs a reboot now
const char *error = "";

// Nothing will bring Bluetooth up this boot, so give the controller's and host stack's BSS/data back to
// the general heap: ~16 KB on obi_gateway_c3, which MQTT, the web server and above all gw_ota_begin()'s
// single contiguous malloc() of a reader image can use instead. esp_bt.h allows this only while the
// controller has never been initialized, so it is one-way until the next reboot -- which is exactly the
// trade wanted here: the heap belongs either to Bluetooth or to everything else, never half to each.
void releaseBleMemory() {
  if (bleReleased) return;
  bleReleased = esp_bt_mem_release(ESP_BT_MODE_BTDM) == ESP_OK;
  if (bleReleased) Serial.printf("[bthome] off -- released Bluetooth RAM to the heap\n");
}

bool reserveCounter(uint64_t high, void *) {
  Preferences p;
  if (!p.begin("obiblecount", false)) return false;
  bool ok = p.putULong64("high", high) == sizeof high;
  p.end();
  return ok;
}

bool loadCounter(bool required) {
  Preferences p;
  if (!p.begin("obiblecount", false)) return false;
  bool ok = p.isKey("high");
  if (!ok && !required) ok = p.putULong64("high", 0) == sizeof(uint64_t);
  uint64_t high = p.getULong64("high", bthome::COUNTER_LIMIT);
  p.end();
  if (!ok || high >= bthome::COUNTER_LIMIT) return false;
  counter = bthome::Counter(high);
  return true;
}

bool saveConfig(const Config &cfg) {
  uint8_t data[24] = {1, uint8_t(cfg.enabled), uint8_t(cfg.hasKey)};
  memcpy(data + 3, cfg.reader, 3);
  data[6] = cfg.stale & 0xFF; data[7] = cfg.stale >> 8;
  memcpy(data + 8, cfg.key, 16);
  Preferences p;
  if (!p.begin("obible", false)) return false;
  bool ok = p.putBytes("config", data, sizeof data) == sizeof data;
  p.end();
  return ok;
}

void stopAdvertising() {
  if (advertising) advertising->stop();
  haveSent = false;
}

// Switched off (or factory-reset) at runtime. Drop the host stack's heap right away; the controller's
// own RAM only comes back if it was never initialized, so once BLE has run this boot the remainder
// waits for a reboot -- reported as "reboot_required".
void shutdownBle() {
  if (advertising) {
    stopAdvertising();
    NimBLEDevice::deinit(true);    // clearAll deletes the advertising object -- drop our pointer too
    advertising = nullptr;
  }
  releaseBleMemory();
}

void fail(const char *message) {
  stopAdvertising();
  error = message;
  Serial.printf("[bthome] %s\n", message);
}

bool encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *plain,
             size_t size, uint8_t *cipher, uint8_t *tag) {
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0)
    rc = mbedtls_ccm_encrypt_and_tag(&ctx, size, nonce, 13, nullptr, 0,
                                    plain, cipher, tag, 4);
  mbedtls_ccm_free(&ctx);
  return rc == 0;
}

bool startBle() {
  if (advertising) return true;
  if (bleReleased) return false;   // its RAM is someone else's now; only a reboot can undo that
  if (!NimBLEDevice::init("OBI-PWR")) return false;
  if (!NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC)) return false;
  advertising = NimBLEDevice::getAdvertising();
  if (!advertising) return false;
  advertising->enableScanResponse(false);
  advertising->setMinInterval(160);
  advertising->setMaxInterval(240);
  return advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
}

bool parseAge(const String &text, uint16_t &age) {
  if (!text.length() || text.length() > 4) return false;
  uint32_t number = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    number = number * 10 + text[i] - '0';
  }
  if (number < 5 || number > 3600) return false;
  age = number;
  return true;
}

bool hasReader(const uint8_t handle[3]) {
  for (int i = 0; i < MAX_READERS; ++i) {
    const Reader &r = readers[i];
    if (r.used && r.assigned && r.devType == 0x10 && !memcmp(r.handle, handle, 3))
      return true;
  }
  return false;
}

void replyError(int code, const char *message) {
  server->send(code, "application/json", String("{\"error\":\"") + message + "\"}");
}

}  // namespace

void bthome_init(WebServer *srv) {
  server = srv;
  stopAdvertising();
  error = "";
  counterReady = false;
  Config loadedConfig;
  uint8_t data[24];
  Preferences p;
  if (!p.begin("obible", false)) { error = "Configuration storage unavailable"; releaseBleMemory(); return; }
  bool exists = p.isKey("config");
  size_t size = p.getBytesLength("config");
  bool loaded = size == sizeof data && p.getBytes("config", data, sizeof data) == sizeof data;
  p.end();
  if (exists && (!loaded || data[0] != 1 || data[1] > 1 || data[2] > 1)) {
    error = "Invalid stored configuration";
    releaseBleMemory();
    return;
  }
  if (loaded) {
    loadedConfig.enabled = data[1]; loadedConfig.hasKey = data[2];
    memcpy(loadedConfig.reader, data + 3, 3);
    loadedConfig.stale = data[6] | (uint16_t(data[7]) << 8);
    memcpy(loadedConfig.key, data + 8, 16);
    if (loadedConfig.stale < 5 || loadedConfig.stale > 3600 ||
        (loadedConfig.enabled && !loadedConfig.hasKey)) {
      error = "Invalid stored configuration";
      releaseBleMemory();
      return;
    }
  }
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) { error = "Bluetooth address unavailable"; releaseBleMemory(); return; }
  counterReady = loadCounter(loadedConfig.hasKey);
  if (!counterReady) error = "Counter unavailable or exhausted";
  portENTER_CRITICAL(&sampleMux);
  config = loadedConfig; sample = Sample{};
  portEXIT_CRITICAL(&sampleMux);
  if (!loadedConfig.enabled || !counterReady) releaseBleMemory();
}

void bthome_capture(const Reader &r) {
  portENTER_CRITICAL(&sampleMux);
  if (config.enabled && !memcmp(config.reader, r.handle, 3)) {
    sample.power = r.power;
    sample.received = r.lastEnergyMs;
    sample.valid = r.used && r.assigned && r.devType == 0x10 && r.haveData &&
                   !r.inBootloader && (r.flags & 1);
    ++sample.generation;
  }
  portEXIT_CRITICAL(&sampleMux);
}

void bthome_forget(const uint8_t handle[3]) {
  portENTER_CRITICAL(&sampleMux);
  if (!memcmp(config.reader, handle, 3)) sample.valid = false;
  portEXIT_CRITICAL(&sampleMux);
}

void bthome_service() {
  Config cfg;
  Sample latest;
  portENTER_CRITICAL(&sampleMux);
  cfg = config; latest = sample;
  portEXIT_CRITICAL(&sampleMux);
  uint32_t now = millis();
  int32_t scaled;
  bool fresh = bthome::sampleFresh(latest.valid, latest.received, now, uint32_t(cfg.stale) * 1000);
  if (!fresh) {
    portENTER_CRITICAL(&sampleMux);
    if (sample.generation == latest.generation) sample.valid = false;
    portEXIT_CRITICAL(&sampleMux);
  }
  if (!cfg.enabled || !cfg.hasKey || error[0] ||
      !fresh ||
      !bthome::powerValue(latest.power, scaled)) {
    if (haveSent) stopAdvertising();
    return;
  }
  uint32_t elapsed = now - lastSend;
  if (haveSent && (elapsed < 100 || (latest.generation == lastGeneration && elapsed < 1000))) return;
  if (!startBle()) { fail("Bluetooth initialization failed"); return; }
  uint32_t sequence;
  if (!counterReady || !counter.take(sequence, reserveCounter, nullptr)) {
    fail("Counter persistence failed or exhausted"); return;
  }
  uint8_t data[bthome::SERVICE_SIZE];
  if (!bthome::powerPacket(latest.power, cfg.key, mac, sequence, data, encrypt)) {
    fail("Packet encryption failed"); return;
  }
  NimBLEAdvertisementData adv;
  bool ok = adv.setFlags(0x06) && adv.setName("OBI-PWR") &&
            adv.setServiceData(NimBLEUUID(uint16_t(0xFCD2)), data + 2, sizeof data - 2);
  now = millis();
  if (!bthome::sampleFresh(latest.valid, latest.received, now, uint32_t(cfg.stale) * 1000)) {
    stopAdvertising(); return;
  }
  uint32_t remaining = uint32_t(cfg.stale) * 1000 - uint32_t(now - latest.received);
  if (remaining < 100) { stopAdvertising(); return; }
  uint32_t duration = remaining < 2000 ? remaining : 2000;
  if (!ok || !advertising->stop() || !advertising->setAdvertisementData(adv) ||
      !advertising->start(duration)) {
    fail("Bluetooth advertising failed"); return;
  }
  haveSent = true;
  lastSend = now; lastGeneration = latest.generation; ++frames;
}

void bthome_status() {
  char address[18], handle[7];
  snprintf(address, sizeof address, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Config cfg;
  Sample latest;
  portENTER_CRITICAL(&sampleMux);
  cfg = config; latest = sample;
  portEXIT_CRITICAL(&sampleMux);
  snprintf(handle, sizeof handle, "%02x%02x%02x", cfg.reader[0], cfg.reader[1], cfg.reader[2]);
  String json = "{\"enabled\":" + String(cfg.enabled ? "true" : "false");
  json += ",\"key_set\":" + String(cfg.hasKey ? "true" : "false");
  json += ",\"advertising\":" + String(advertising && advertising->isAdvertising() ? "true" : "false");
  json += ",\"reboot_required\":" + String(cfg.enabled && bleReleased ? "true" : "false");
  json += ",\"mac\":\"" + String(address) + "\",\"reader\":\"" + String(handle) + "\"";
  json += ",\"stale_seconds\":" + String(cfg.stale) + ",\"frames\":" + String(frames);
  json += ",\"sample_age_ms\":" + (latest.valid ? String(uint32_t(millis() - latest.received)) : String("null"));
  json += ",\"error\":\"" + String(error) + "\"}";
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", json);
}

void bthome_configure() {
  Config next;
  portENTER_CRITICAL(&sampleMux);
  next = config;
  portEXIT_CRITICAL(&sampleMux);
  for (int i = 0; i < server->args(); ++i) {
    String name = server->argName(i);
    if (name != "enabled" && name != "reader" && name != "key" && name != "stale_seconds") {
      replyError(400, "Unknown setting"); return;
    }
  }
  if (server->hasArg("enabled")) {
    String value = server->arg("enabled");
    if (value != "0" && value != "1") { replyError(400, "Enabled must be 0 or 1"); return; }
    next.enabled = value == "1";
  }
  if (server->hasArg("reader") && !bthome::parseHex(server->arg("reader").c_str(), next.reader, 3)) {
    replyError(400, "Reader requires six hex characters"); return;
  }
  if (server->hasArg("key")) {
    if (!bthome::parseHex(server->arg("key").c_str(), next.key, 16)) {
      replyError(400, "Key requires 32 hex characters"); return;
    }
    next.hasKey = true;
  }
  if (server->hasArg("stale_seconds") && !parseAge(server->arg("stale_seconds"), next.stale)) {
    replyError(400, "Stale timeout must be 5–3600 seconds"); return;
  }
  if (next.enabled && (!next.hasKey || !hasReader(next.reader))) {
    replyError(400, "Select an assigned meter and provide a key"); return;
  }
  if (next.enabled && !counterReady) { replyError(503, "Counter unavailable; reboot and inspect storage"); return; }
  if (!saveConfig(next)) { replyError(503, "Could not save configuration"); return; }
  stopAdvertising();
  portENTER_CRITICAL(&sampleMux);
  config = next; sample = Sample{};
  portEXIT_CRITICAL(&sampleMux);
  if (!next.enabled) shutdownBle();   // off again: hand the heap back instead of holding it idle
  // Enabling after the RAM went to the heap can only take effect on the next boot. The configuration is
  // stored either way; "reboot_required" and this message say what is still missing.
  error = (next.enabled && bleReleased) ? "Reboot required to start Bluetooth" : "";
  bthome_status();
}

void bthome_reset() {
  portENTER_CRITICAL(&sampleMux);
  config = Config{}; sample = Sample{};
  portEXIT_CRITICAL(&sampleMux);
  Preferences p;
  if (p.begin("obible", false)) { p.clear(); p.end(); }
  error = "";
  shutdownBle();
  // Preserve counters across factory resets.
}

#endif
