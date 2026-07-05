// OBI / heyOBI LoRa mini-gateway  —  ESP32 + SX1262 (any board via board_config.h)
// ---------------------------------------------------------------------------
// Acts as the reader's gateway so it pairs & ECDH-key-exchanges with US, letting
// us hold the per-device TEA key and read the energy payload in the clear. Both reader
// generations do the same ECDH -> TEA-ECB (the old v32 reader / cloud "1.0.1" as well as
// 1.2.x); only the frame/command layout differs, NOT the crypto.
//
// Flow (all reversed from firmware 1.2.1):
//   - send the 1 Hz time beacon (cmd 15, broadcast) carrying OUR 6-byte gateway id
//   - send scan requests (cmd 36) -> readers answer with an announce (cmd 17)
//   - on announce: bind the reader (cmd 59 = [crc16(gwid)][gwid]) -> reader stores our id
//   - reader then starts ECDH (cmd 32, its 64-byte P-256 pubkey); we reply with ours
//     and derive TEA key = first 16 bytes of the shared X
//   - reader sends energy (cmd 37/39/19/22); we TEA-decrypt and print it
//
// OPERATIONAL: a reader syncs to ONE gateway beacon at a time. Power OFF the real
// Obi bridge (or move the reader out of its range) and factory-reset / re-pair the
// reader so it binds to this ESP32.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <RadioLib.h>
#include "board_config.h"
#include "obi_proto.h"
#include "obi_ecdh.h"
#include "reader.h"
#include "gateway_web.h"
#include "flash_dbg.h"
#include "status_led.h"
#include <Preferences.h>

// ---- reversed OBI radio configuration --------------------------------------
#define OBI_FREQ_MHZ   869.5f
#define OBI_BW_KHZ     500.0f
#define OBI_SF         7
#define OBI_CR         5
#define OBI_SYNCWORD   0x12          // RadioLib private -> SX126x 0x1424
#define OBI_TXPWR_DBM  22
#define OBI_PREAMBLE   12

// ---- our gateway identity (any 6 bytes; the reader stores it on bind) -------
const uint8_t GWID[6] = { 'O', 'B', 'I', 'E', 'S', 'P' };

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);

// ---- per-reader state (struct in reader.h, shared with the web/MQTT module) --
const int MAX_READERS = 64;   // ~92 B/entry -> ~6 KB RAM for 64; the real ceiling is LoRa airtime, not memory
Reader readers[MAX_READERS];

// An UNBOUND reader (never accepted onto this gateway) that goes silent this long is almost certainly a
// mis-decoded frame — bogus ids like FFFFFD that never really existed — so it is auto-pruned (see loop()).
static const uint32_t READER_STALE_MS = 400000;   // ~400 s

static uint8_t  g_ourPub[64];        // our static ECDH public key (generated once)
static bool     g_ecdhReady = false;

static Reader *findReader(const uint8_t h[3]) {
  for (auto &r : readers) if (r.used && !memcmp(r.handle, h, 3)) return &r;
  return nullptr;
}

// Handles that are never a real reader — mis-decode/noise artifacts that otherwise show up as phantom
// entries (e.g. FFFFFD). We drop these frames entirely instead of relying on the stale-prune timeout.
static inline bool isIgnoredHandle(const uint8_t h[3]) {
  return h[0] == 0xFF && h[1] == 0xFF && h[2] == 0xFD;   // FFFFFD
}
// UUIDs persist in NVS keyed by handle, so a reader keeps its UUID across ESP32 reboots
// once we've seen it announce (cmd 17/35) at least once.
static Preferences g_uuidStore;
static void uuidKey(const uint8_t h[3], char *out) { sprintf(out, "%02x%02x%02x", h[0], h[1], h[2]); }
static void saveUuid(const uint8_t h[3], const uint8_t uuid[16]) {
  char k[8]; uuidKey(h, k);
  g_uuidStore.begin("obiuuid", false); g_uuidStore.putBytes(k, uuid, 16); g_uuidStore.end();
}
static bool loadUuid(const uint8_t h[3], uint8_t uuid[16]) {
  char k[8]; uuidKey(h, k);
  g_uuidStore.begin("obiuuid", true);
  size_t n = g_uuidStore.getBytes(k, uuid, 16); g_uuidStore.end();
  return n == 16;
}

// Which readers the user has accepted onto THIS gateway (persisted). Until a reader is assigned it is
// only tracked/shown (greyed) and NOT bound or key-exchanged, so two gateways don't fight over it.
// NOTE: use a LOCAL Preferences per call — saveAssigned runs on the web task (core 0) while loadAssigned
// runs on the LoRa task (core 1); a shared Preferences object's begin() returns false when already open on
// the other core, so the write would silently hit a read-only/closed handle and never persist.
static void saveAssigned(const uint8_t h[3], bool on) {
  char k[8]; uuidKey(h, k);
  Preferences p; p.begin("obiassign", false);
  if (on) p.putBool(k, true); else p.remove(k);
  p.end();
  Serial.printf("[assign] %s %s\n", on ? "bind" : "unbind", k);
}
static bool loadAssigned(const uint8_t h[3]) {
  char k[8]; uuidKey(h, k);
  Preferences p; p.begin("obiassign", true);
  bool v = p.getBool(k, false); p.end();
  return v;
}

// Auto-pair window: while active, every reader that announces is accepted automatically.
static uint32_t g_pairUntil = 0;
static bool pairWindowActive() { return g_pairUntil && (int32_t)(millis() - g_pairUntil) < 0; }

static Reader *addReader(const uint8_t h[3]) {
  if (isIgnoredHandle(h)) return nullptr;   // never create a slot for a known phantom id
  Reader *r = findReader(h);
  if (r) return r;
  for (auto &x : readers) if (!x.used) {
    x = Reader{}; x.used = true; x.lastSeenMs = millis(); memcpy(x.handle, h, 3);   // stamp now so a fresh entry isn't instantly pruned
    if (loadUuid(h, x.uuid)) x.haveUuid = true;     // restore a previously-seen UUID
    x.assigned = loadAssigned(h);                   // restore the accept flag (auto-binds on sight)
    return &x;
  }
  return nullptr;
}

// Should we bind/key/ack this reader? Only if it's assigned — or auto-accept it during a pair window.
static bool acceptReader(Reader *r) {
  if (!r) return false;
  if (r->assigned) return true;
  if (pairWindowActive()) {
    r->assigned = true; saveAssigned(r->handle, true);
    Serial.printf("  [pair] auto-assigned %02X%02X%02X (pair-all window)\n", r->handle[0], r->handle[1], r->handle[2]);
    return true;
  }
  return false;
}

// ---- radio RX flag + LoRa-task wakeup --------------------------------------
// The LoRa RX/TX runs in its OWN high-priority FreeRTOS task (not loop()), so on the single-core C3 it
// preempts the web/MQTT task and answers a reader's OTA block request inside its short RX window — the
// same reason the stock firmware is fast on the C3. The DIO1 ISR wakes the task immediately.
volatile bool g_rx = false;
volatile uint32_t g_reqMicros = 0;   // micros() at the last DIO1 IRQ (RxDone) — for measuring response latency
static SemaphoreHandle_t g_loraSem = nullptr;
void IRAM_ATTR onDio1() {
  g_rx = true;
  g_reqMicros = micros();
  if (g_loraSem) { BaseType_t hpw = pdFALSE; xSemaphoreGiveFromISR(g_loraSem, &hpw); if (hpw) portYIELD_FROM_ISR(); }
}

static void hexdump(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) { if (p[i] < 16) Serial.print('0'); Serial.print(p[i], HEX); Serial.print(' '); }
}

// ---- live radio log ring buffer (served by the /radio web page) ------------
static const int RAW_MAX = 255;   // store the whole packet (LoRa RX buffer is 256 B) so nothing is truncated
struct Rlog { uint32_t seq, ms; char dir; uint8_t h[3]; int16_t cmd, len, rssi; char note[20];
              uint8_t rawLen; uint8_t raw[RAW_MAX]; };
static const int RLOG_N = 96;     // ~96 * ~280 B ≈ 27 KB RAM; client keeps its own 800-row history anyway
static Rlog g_rl[RLOG_N];
static volatile uint32_t g_rlSeq = 0;
static int g_rlHead = 0;
void gw_radio_log(char dir, const uint8_t h[3], int cmd, int len, int rssi, const char *note,
                  const uint8_t *raw, int rawLen) {
  Rlog &e = g_rl[g_rlHead];
  e.seq = ++g_rlSeq; e.ms = millis(); e.dir = dir;
  if (h) memcpy(e.h, h, 3); else { e.h[0] = e.h[1] = e.h[2] = 0; }
  e.cmd = (int16_t)cmd; e.len = (int16_t)len; e.rssi = (int16_t)rssi;
  strncpy(e.note, note ? note : "", sizeof e.note - 1); e.note[sizeof e.note - 1] = 0;
  int n = rawLen < 0 ? 0 : (rawLen > RAW_MAX ? RAW_MAX : rawLen);
  e.rawLen = (uint8_t)n;
  if (raw && n) memcpy(e.raw, raw, n);
  g_rlHead = (g_rlHead + 1) % RLOG_N;
}
String gw_radio_json(uint32_t since) {
  String j = "{\"seq\":" + String(g_rlSeq) + ",\"e\":[";
  bool first = true;
  for (int i = 0; i < RLOG_N; i++) {
    Rlog &e = g_rl[i];
    if (e.seq == 0 || e.seq <= since) continue;
    char hs[7]; snprintf(hs, sizeof hs, "%02X%02X%02X", e.h[0], e.h[1], e.h[2]);
    if (!first) j += ",";
    first = false;
    j += "{\"s\":" + String(e.seq) + ",\"t\":" + String(e.ms) + ",\"d\":\"" + String(e.dir) +
         "\",\"h\":\"" + String(hs) + "\",\"c\":" + String(e.cmd) + ",\"l\":" + String(e.len) +
         ",\"r\":" + String(e.rssi) + ",\"n\":\"" + String(e.note) + "\",\"b\":\"";
    char bb[3];
    for (int k = 0; k < e.rawLen; k++) { snprintf(bb, sizeof bb, "%02X", e.raw[k]); j += bb; }
    j += "\"}";
  }
  return j + "]}";
}

// transmit a frame, then go back to RX
static void txFrame(const uint8_t *buf, size_t len, const char *what) {
  int st = radio.transmit((uint8_t *)buf, len);
  radio.startReceive();
  g_rx = false;                 // discard the TxDone that also pulses DIO1
  gw_radio_log('T', buf, -1, (int)len, 0, what, buf, (int)len);   // buf[0..2] = target handle (plaintext)
  Serial.printf("  TX %-6s len=%d -> %d\n", what, (int)len, st);   // note carries offset+latency for OTA blocks
}

// pull the 16-byte UUID + type + version out of an announce (XOR-decoded payload).
//   cmd 35 (1.2.x): [0:2]crc16 [2]type [3:19]uuid [19]softver [20]hardver [21]battery
//   cmd 17 (legacy): [0:16]uuid [16]battery
static void storeAnnounce(Reader *r, uint8_t cmd, const uint8_t *pl, size_t plen) {
  if (!r) return;
  if (cmd == 35 && plen >= 22) {
    r->devType = pl[2];
    memcpy(r->uuid, pl + 3, 16); r->haveUuid = true; saveUuid(r->handle, r->uuid);
    r->softver = pl[19]; r->hardver = pl[20]; r->battery_mV = 20 * pl[21];
  } else if (cmd == OBI_CMD_READER_ANNOUNCE && plen >= 17) {
    memcpy(r->uuid, pl, 16); r->haveUuid = true; saveUuid(r->handle, r->uuid);
    r->battery_mV = 20 * pl[16];
  }
  r->lastSeenMs = millis();
}

// ---- web-UI hooks (declared in reader.h) -----------------------------------
uint32_t gw_uptime_s() { return millis() / 1000; }
void gw_request_interval(const uint8_t handle[3], uint16_t seconds) {
  for (auto &r : readers)
    if (r.used && !memcmp(r.handle, handle, 3)) { r.setInterval = seconds; return; }
}
// Drop a reader from the list AND remove its binding, so deleting truly un-adopts it: the next frame
// re-creates it via addReader() as unassigned (greyed), not auto-bound. The persisted UUID is left in NVS
// so a real reader keeps its identity if you re-bind it later.
bool gw_delete_reader(const uint8_t handle[3]) {
  saveAssigned(handle, false);        // remove the persisted binding
  bool found = false;
  for (auto &r : readers)
    if (r.used && !memcmp(r.handle, handle, 3)) { r.assigned = false; r.haveKey = false; r.used = false; found = true; }
  return found;
}
// Accept (or drop) a reader onto this gateway. Persisted, so it auto-binds/keys on the next announce and
// after reboots. Dropping also forgets its key so it stops being maintained.
bool gw_assign_reader(const uint8_t handle[3], bool on) {
  saveAssigned(handle, on);
  for (auto &r : readers)
    if (r.used && !memcmp(r.handle, handle, 3)) {
      r.assigned = on;
      if (!on) { r.haveKey = false; r.decoded = false; }
      return true;
    }
  return true;   // persisted even if the reader isn't currently in the live list
}
// Open a window during which every announcing reader is auto-accepted (default 3 min from the web button).
void gw_pair_all(uint16_t seconds) {
  g_pairUntil = millis() + (uint32_t)seconds * 1000;
  Serial.printf("[pair] auto-pair window open for %u s\n", seconds);
}
uint32_t gw_pair_remaining_s() { return pairWindowActive() ? (g_pairUntil - millis()) / 1000 : 0; }

// default upload interval (seconds) when the user hasn't set one for a reader
#define OBI_DEFAULT_INTERVAL 10

// Energy ACK (cmd 38 for cmd37-family, cmd 40 for the live/39 family). The reader WAITS for this
// after every report; without it, it retries every ~5-6 s. The ack carries the upload interval
// (reader applies it directly) and a firmware version (== current 57 = no OTA; != 57 triggers OTA).
// Wire (6 B) reversed from sub_4201471C / sub_5AE4: [crc_lo][crc_hi][version][int_hi][int_lo][flag],
// crc16 over bytes [2..6]. Not TEA — just the standard frame XOR on top.
static void sendEnergyAck(Reader *r, uint8_t rxcmd, uint8_t version) {
  if (!r->haveKey) return;                          // the ack is TEA-encrypted with the reader's key
  uint16_t interval = r->setInterval ? r->setInterval : OBI_DEFAULT_INTERVAL;
  // plaintext: 6 meaningful bytes padded to 8 (TEA block). CRC16 covers only bytes [2..6].
  //   [crc_lo][crc_hi][version][int_hi][int_lo][flag][pad][pad]
  uint8_t a[8] = {0};
  a[2] = version;
  a[3] = interval >> 8; a[4] = interval & 0xFF; a[5] = 0;
  uint16_t c = obi_crc16(a + 2, 4);
  a[0] = c & 0xFF; a[1] = c >> 8;
  obi_tea_ecb_encrypt(a, 8, r->key);                // reader TEA-decrypts it (sub_9C30) before the CRC check
  uint8_t cmd = (rxcmd == OBI_CMD_ENERGY_LIVE_D || rxcmd == OBI_CMD_ENERGY_RT || rxcmd == 25) ? 40 : 38;
  uint8_t f[16];
  size_t n = obi_build_frame(f, r->handle, cmd, a, sizeof(a));   // frame-level XOR on top
  txFrame(f, n, "ack");
}

// Legacy "_c" energy ACK — the OLD readers (cmd 24 / 25, softver 3x) ack on cmd 24->56, 25->57.
// FULLY reversed from the v1.0.1 reader handler sub_B7A0 (registered for cmd 56/57 via sub_5FD0):
// the reader REQUIRES the ack **TEA-encrypted**, length a multiple of 8 (it rejects anything else),
// decrypts with its ECDH key, then checks:
//   payload[0] = VERSION   -> ==0 or ==32(own) = no-op; != own for 3 consecutive acks => SCB_AIRCR reset
//                             into the bootloader to pull firmware (sub_A930).
//   payload[3..4] = crc16 (OBI split-table = obi_crc16) over payload[0..2]  (a1[3]=lo, a1[4]=hi).
// So: [version][0xFF][abs(rssi)][crc_lo][crc_hi][0][0][0], TEA-encrypt, send on cmd 56/57.
static void sendLegacyAck(Reader *r, uint8_t rxcmd, uint8_t version, float rssi) {
  if (!r->haveKey) return;                           // ack is TEA-encrypted with the reader's key
  uint8_t b[8] = {0};
  b[0] = version;
  // b[1] = interval byte. The v38-family meter reader applies interval = 2 * b[1] seconds (sub_600C;
  // byte 0 or 255 -> its 300 s default). The stock value here was 0xFF (=300 s); send the user-configured
  // interval instead so the dashboard interval control also works for the cmd-24/25 readers. The old
  // v1.0.1/v32 readers ignore b[1] (they only check version + crc), so this stays safe for them; the CRC
  // below is recomputed over the new bytes either way.
  uint16_t secs = r->setInterval ? r->setInterval : OBI_DEFAULT_INTERVAL;
  uint16_t half = secs / 2;                            // firmware doubles it: interval = 2 * byte
  if (half < 1)   half = 1;                            // 0/255 would select the reader's 300 s default
  if (half > 254) half = 254;
  b[1] = (uint8_t)half;
  b[2] = (uint8_t)(rssi < 0 ? -rssi : rssi);
  uint16_t c = obi_crc16(b, 3);
  b[3] = c & 0xFF; b[4] = c >> 8;
  obi_tea_ecb_encrypt(b, 8, r->key);                 // reader TEA-decrypts (sub_A1E0) before the CRC check
  uint8_t cmd = (rxcmd == 25) ? 57 : 56;
  uint8_t f[16];
  size_t n = obi_build_frame(f, r->handle, cmd, b, sizeof(b));
  txFrame(f, n, "ack-v32");
}

// Raw (unencrypted) "completedata" ACK for a plaintext-generation reader (no ECDH / no TEA — payloads
// are only handle-XOR'd, key = (h0+h1+h2)&0xFF = obi_xor_key). Fully reversed from reader_v1.0.0
// sub_B8DC (softver 24) and reader_v31.0.0 completedata_on_ack_ota (softver 31): the handler is
// registered on wire cmd (energy_cmd | 0x20) — so v24 (energy cmd 22) listens on cmd 54, v31 (energy
// cmd 19) on cmd 51. Payload is EXACTLY 3 bytes [version][interval_byte][pad]:
//   - interval: applied as interval = 2 * interval_byte seconds (sub_63A0; byte 0 or 255 -> 300 s).
//   - version:  0 or the reader's own softver = no-op; ANY other value seen 3x in a row => the reader
//               arms the 0x3782 handoff record + SCB_AIRCR reset into the bootloader to pull an OTA.
// No key needed — the outer handle-XOR (obi_build_frame) already matches what the reader expects.
static void sendCompletedataRaw(Reader *r, uint8_t ackCmd, uint8_t version) {
  uint16_t secs = r->setInterval ? r->setInterval : OBI_DEFAULT_INTERVAL;
  uint16_t half = secs / 2;                          // firmware doubles it: interval = 2 * byte
  if (half < 1)   half = 1;                           // 0/255 would select the reader's 300 s default
  if (half > 254) half = 254;
  uint8_t pl[3] = { version, (uint8_t)half, 0x00 };   // [version][interval_byte][pad]
  uint8_t f[16];
  size_t n = obi_build_frame(f, r->handle, ackCmd, pl, sizeof(pl));
  txFrame(f, n, version ? "otaCd" : "intervalCd");
}

// ============ reader firmware OTA over LoRa (cmd 33 request -> cmd 34 serve) ============
// The reader's MAIN firmware, on 3 ACKs advertising a version != its current, resets into its
// BOOTLOADER, which then PULLS the image 64 bytes at a time (cmd 33). We serve cmd 34 exactly like
// the real gateway (lora_p0_cmd33_handler): metadata for offset 0xFFFFFFFF, else a 64-byte block.
// XOR-framed (not TEA). The reader's bootloader validates the image before flashing, so a wrong
// serve is rejected — it doesn't run a corrupt image.
static uint8_t *g_ota = nullptr;
static uint32_t g_otaSize = 0, g_otaGot = 0, g_otaServed = 0;
static uint8_t  g_otaTarget[3] = {0};
static uint8_t  g_otaVersion = 1;
static bool     g_otaActive = false;

bool gw_ota_begin(const uint8_t handle[3], uint32_t total, uint8_t version) {
  if (g_ota) { free(g_ota); g_ota = nullptr; }
  if (!total || total > 512u * 1024) return false;
  g_ota = (uint8_t *)malloc(total);
  if (!g_ota) return false;
  g_otaSize = total; g_otaGot = 0; g_otaServed = 0;
  memcpy(g_otaTarget, handle, 3); g_otaVersion = version ? version : 1; g_otaActive = false;
  Serial.printf("[ota] begin %u B -> %02X%02X%02X ver %u\n", total, handle[0], handle[1], handle[2], g_otaVersion);
  return true;
}
void gw_ota_write(const uint8_t *data, uint32_t len) {
  if (g_ota && g_otaGot + len <= g_otaSize) { memcpy(g_ota + g_otaGot, data, len); g_otaGot += len; }
}
bool gw_ota_arm() {
  if (!g_ota || g_otaGot != g_otaSize) { Serial.printf("[ota] arm fail %u/%u\n", g_otaGot, g_otaSize); return false; }
  g_otaActive = true;
  Serial.println("[ota] armed — advertising new version in the ACK; reader will reset & pull");
  return true;
}
void gw_ota_cancel() { g_otaActive = false; if (g_ota) { free(g_ota); g_ota = nullptr; } g_otaSize = 0; }
bool gw_ota_active() { return g_otaActive; }
uint32_t gw_ota_size() { return g_otaSize; }
uint32_t gw_ota_progress() { return g_otaServed > g_otaSize ? g_otaSize : g_otaServed; }
void gw_ota_target(uint8_t out[3]) { memcpy(out, g_otaTarget, 3); }

// serve one cmd-33 request. request payload = [f0][deviceType][f2][offset:4 big-endian]
static void serveOtaRequest(const uint8_t handle[3], const uint8_t *req, size_t rlen) {
  if (rlen < 7 || !g_ota) return;
  uint8_t type = req[1];
  const uint8_t *ob = req + 3;
  uint32_t pos = ((uint32_t)ob[0] << 24) | (ob[1] << 16) | (ob[2] << 8) | ob[3];
  if (pos == 0xFFFFFFFF) {                             // metadata
    uint8_t m[13] = {0};
    m[0] = type; m[1] = m[2] = m[3] = m[4] = 0xFF; m[5] = 0; m[6] = g_otaVersion;
    m[7] = g_otaSize >> 24; m[8] = g_otaSize >> 16; m[9] = g_otaSize >> 8; m[10] = g_otaSize;
    uint16_t hc = obi_crc16(g_ota, g_otaSize);
    m[11] = hc & 0xFF; m[12] = hc >> 8;
    uint8_t f[24]; size_t n = obi_build_frame(f, handle, 34, m, sizeof(m));
    txFrame(f, n, "ota-meta");
    Serial.printf("[ota] meta -> size %u ver %u\n", g_otaSize, g_otaVersion);
  } else {                                             // 64-byte block
    uint8_t b[72] = {0};
    b[0] = type; b[1] = ob[0]; b[2] = ob[1]; b[3] = ob[2]; b[4] = ob[3]; b[5] = g_otaVersion;
    uint32_t avail = pos < g_otaSize ? (g_otaSize - pos < 64 ? g_otaSize - pos : 64) : 0;
    if (avail) memcpy(b + 6, g_ota + pos, avail);
    uint16_t bc = obi_crc16(b + 6, 64);
    b[70] = bc & 0xFF; b[71] = bc >> 8;
    uint8_t f[80]; size_t n = obi_build_frame(f, handle, 34, b, sizeof(b));
    char note[16]; snprintf(note, sizeof note, "b@%u", (unsigned)pos);
    txFrame(f, n, note);
    if (pos < g_otaSize) { uint32_t s = pos + 64 > g_otaSize ? g_otaSize : pos + 64; if (s > g_otaServed) g_otaServed = s; }
    if (g_otaServed >= g_otaSize) {                   // whole image served -> stop advertising the version
      g_otaActive = false;
      Serial.println("[ota] full image served — disarmed (reader will validate & flash)");
    }
  }
}

// Wait until this many µs after the reader's request (RxDone) before sending the OTA block, so the reader
// has finished its TX->RX turnaround and is actually listening. Answering too early (the C3 does it in
// ~1.8 ms) means the block preamble arrives before the reader's RX is open -> it misses it -> 1 s retry.
#ifndef OTA_RESP_DELAY_US
#define OTA_RESP_DELAY_US 8000
#endif
static inline void otaRespWait() { while ((uint32_t)(micros() - g_reqMicros) < OTA_RESP_DELAY_US) {} }

// LEGACY OTA used by the reader BOOTLOADER. Response cmd = reqCmd | 0x20 (cmd21->53, cmd20->52). type 0x10.
// request 6B = [f0][f1][offset:4 LE]; readPos = big-endian of those 4 bytes.
//   metadata (offset FFFFFFFF), 12B (both): [offset:4=FF][field][version][size:4 BE][crc16(image):2]
//   cmd21 block, 71B: [offset:4 echo][f1=req[1]][crc16(block):2][64B block]   (crc BEFORE block)
//   cmd20 block, 69B: [offset:4 echo][version][64B block]                      (NO crc)
static void serveOtaLegacy(const uint8_t handle[3], uint8_t reqCmd, const uint8_t *req, size_t rlen) {
  if (rlen < 6 || !g_ota) return;
  uint8_t rc = reqCmd | 0x20;
  const uint8_t *ob = req + 2;
  uint32_t pos = ((uint32_t)ob[0] << 24) | (ob[1] << 16) | (ob[2] << 8) | ob[3];   // BE read position
  if (pos == 0xFFFFFFFF) {                            // metadata (identical for cmd 20 & 21)
    uint8_t m[12] = {0};
    m[0] = m[1] = m[2] = m[3] = 0xFF; m[4] = 0; m[5] = g_otaVersion;
    m[6] = g_otaSize >> 24; m[7] = g_otaSize >> 16; m[8] = g_otaSize >> 8; m[9] = g_otaSize;
    uint16_t c = obi_crc16(g_ota, g_otaSize);
    m[10] = c & 0xFF; m[11] = c >> 8;
    uint8_t f[24]; size_t n = obi_build_frame(f, handle, rc, m, sizeof(m));
    txFrame(f, n, "ota-meta");
    Serial.printf("[ota%u] meta size %u ver %u crc %04x\n", reqCmd, g_otaSize, g_otaVersion, c);
  } else {
    uint32_t avail = pos < g_otaSize ? (g_otaSize - pos < 64 ? g_otaSize - pos : 64) : 0;
    uint8_t b[71] = {0}; size_t blen;
    b[0] = ob[0]; b[1] = ob[1]; b[2] = ob[2]; b[3] = ob[3];   // echo offset
    if (reqCmd == 21) {                                       // 71B: [off:4][f1][crc:2][64B]
      b[4] = req[1];
      if (avail) memcpy(b + 7, g_ota + pos, avail);
      uint16_t c = obi_crc16(b + 7, 64);
      b[5] = c & 0xFF; b[6] = c >> 8;
      blen = 71;
    } else {                                                  // cmd 20 -> 69B: [off:4][ver][64B]
      b[4] = g_otaVersion;
      if (avail) memcpy(b + 5, g_ota + pos, avail);
      blen = 69;
    }
    uint8_t f[80]; size_t n = obi_build_frame(f, handle, rc, b, blen);
    uint32_t lat = (uint32_t)(micros() - g_reqMicros);        // RxDone -> just before TX = our response latency
    char note[24]; snprintf(note, sizeof note, "b@%u %uus", (unsigned)pos, (unsigned)lat);
    txFrame(f, n, note);
    if (pos < g_otaSize) { uint32_t s = pos + 64 > g_otaSize ? g_otaSize : pos + 64; if (s > g_otaServed) g_otaServed = s; }
    Serial.printf("[ota%u] @%u  %u/%u\n", reqCmd, pos, g_otaServed, g_otaSize);
  }
}

// ---- outgoing frames -------------------------------------------------------
static uint16_t g_beaconCtr = 0;

static void sendBeacon() {
  uint8_t pl[14];
  memcpy(pl, GWID, 6);
  pl[6] = 0xFF; pl[7] = 0x00; pl[8] = 0x00;
  uint32_t up = millis() / 1000;
  pl[9] = up >> 16; pl[10] = up >> 8; pl[11] = up;        // coarse uptime (reader only checks gwid)
  pl[12] = g_beaconCtr >> 8; pl[13] = g_beaconCtr; g_beaconCtr++;
  uint8_t f[32];
  size_t n = obi_build_frame(f, OBI_BROADCAST, OBI_CMD_TIME_BEACON, pl, sizeof(pl));
  txFrame(f, n, "beacon");
}

static void sendScan() {
  uint8_t f[8];
  size_t n = obi_build_frame(f, OBI_BROADCAST, OBI_CMD_SCAN_REQ, nullptr, 0);
  txFrame(f, n, "scan");
}

static void sendBind(Reader *r) {
  uint8_t pl[8];
  uint16_t crc = obi_crc16(GWID, 6);
  pl[0] = crc & 0xFF; pl[1] = crc >> 8;                   // reader checks (p0<<8|p1) == its crc16(gwid)
  memcpy(pl + 2, GWID, 6);
  uint8_t f[16];
  size_t n = obi_build_frame(f, r->handle, OBI_CMD_BIND, pl, sizeof(pl));
  txFrame(f, n, "bind");
  r->lastBind = millis();
}

// The legacy v32 reader (cloud "1.0.1") announces with cmd 17 and expects an empty cmd 49 ack
// ("dealBind old cmd"). This ack is a plaintext control frame (only the outer frame XOR) — the reader
// still runs the ECDH exchange (cmd 32) afterwards and TEA-encrypts its energy, exactly like 1.2.x.
// cmd 49 = OBI old-bind ack.
static void sendActivateOld(Reader *r) {
  uint8_t f[8];
  size_t n = obi_build_frame(f, r->handle, 49, nullptr, 0);
  txFrame(f, n, "act49");
  r->lastBind = millis();
}

// 1.0.x reconnect: reader sends cmd 18, gateway replies cmd 50 = [interval-map, rssi]
// (lora_p0_cmd18_handler). The map byte is 0 on a fresh gateway; rssi is what we measured.
static void sendReconnectAck(Reader *r, float rssi) {
  uint8_t pl[2] = { 0x00, (uint8_t)(int8_t)rssi };
  uint8_t f[8];
  size_t n = obi_build_frame(f, r->handle, 50, pl, sizeof(pl));
  txFrame(f, n, "recon50");
  r->lastBind = millis();
}

static void sendEcdhReply(Reader *r) {
  uint8_t f[80];
  size_t n = obi_build_frame(f, r->handle, OBI_CMD_ECDH, g_ourPub, 64);
  txFrame(f, n, "ecdh");
}

// addressed scan ack — a fresh 1.2.x reader announces (cmd 35) then waits ~300 ms for cmd 36
// to advance from its scan phase into the bind phase (reader sub_5968 -> sub_831C). Our periodic
// broadcast cmd 36 misses that window, so we send an addressed one right after the announce.
static void sendScanAck(Reader *r) {
  uint8_t f[8];
  size_t n = obi_build_frame(f, r->handle, OBI_CMD_SCAN_REQ, nullptr, 0);   // cmd 36
  txFrame(f, n, "scan36");
  r->lastBind = millis();
}

// A reader announces via one of several commands depending on generation + state:
//   cmd 17 (legacy announce), cmd 18 (legacy reconnect), cmd 35 (1.2.x DevicesScan announce),
//   cmd 58 (1.2.x reconnect). We don't always know which, so send every ack — the reader
//   acts on the one it understands and ignores the rest.
static void sendPairAcks(Reader *r, float rssi) {
  sendScanAck(r);            // cmd 36  (advance a fresh 1.2.x reader: scan phase -> bind phase)
  sendBind(r);               // cmd 59  (1.2.x bind: [crc16(gwid)][gwid])
  sendActivateOld(r);        // cmd 49  (legacy announce ack)
  sendReconnectAck(r, rssi); // cmd 50  (legacy reconnect ack)
}

// ---- energy parse (exact layout from gateway meter_parse_payload) ----------
// [0:2] crc16(pl[2:22]) BE  [2] softver  [3] hardver  [4] battery(*20 mV)
// [5] flags: b0 infrared, b1 lowpower   [6:10] import energy (BE u32)
// [10:14] export energy   [14:18] power (BE u32, 0x7FFFFFFF = n/a)  [18:22] time/interval
static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
// quiet CRC check (1.2.x layout): does this candidate plaintext have a valid internal crc16?
static bool energyValid(const uint8_t *pl, size_t n) {
  if (n < 22) return false;
  uint16_t stored = (pl[0] << 8) | pl[1];
  uint16_t calc   = obi_crc16(pl + 2, 20);
  return stored == calc || stored == (uint16_t)((calc << 8) | (calc >> 8));
}
// plausibility check for the legacy layout (no crc): sane hardver + battery in range
static bool legacyValid(const uint8_t *pl, size_t n) {
  if (n < 16) return false;
  uint16_t mv = 20 * pl[2];
  return pl[1] < 50 && mv >= 1500 && mv <= 4500;
}
// legacy 3x.x / 1.0.x layout (gateway sub_4200BCFC): softver[0] hardver[1] battery[2]
// flags[3] (b0 ir, b1 lowpower, b2 timesync) pos[4:8]BE neg[8:12]BE power[12:16]BE. No leading crc.
static bool printEnergyOld(const uint8_t *pl, size_t n) {
  if (n < 16) return false;
  bool sane = pl[0] < 200 && pl[1] < 200;
  Serial.printf("    [legacy] softver=%u hardver=%u battery=%u mV infrared=%u lowpower=%u timesync=%u\n",
                pl[0], pl[1], 20 * pl[2], pl[3] & 1, (pl[3] >> 1) & 1, (pl[3] >> 2) & 1);
  Serial.printf("    [legacy] pos_power=%lu neg_power=%lu power=%lu\n",
                (unsigned long)rd_be32(pl + 4), (unsigned long)rd_be32(pl + 8),
                (unsigned long)rd_be32(pl + 12));
  return sane;
}
static bool printEnergy(const uint8_t *pl, size_t n) {
  if (n < 22) { Serial.println("    (payload too short)"); return false; }
  uint16_t crcStored = (pl[0] << 8) | pl[1];
  uint16_t crcCalc   = obi_crc16(pl + 2, 20);
  bool crcOk = crcStored == crcCalc || crcStored == (uint16_t)((crcCalc << 8) | (crcCalc >> 8));
  uint8_t  flags   = pl[5];
  uint32_t import  = rd_be32(pl + 6);
  uint32_t export_ = rd_be32(pl + 10);
  uint32_t power   = rd_be32(pl + 14);
  Serial.printf("    crc=%s softver=%u hardver=%u battery=%u mV infrared=%u lowpower=%u\n",
                crcOk ? "ok" : "BAD", pl[2], pl[3], 20 * pl[4], flags & 1, (flags >> 1) & 1);
  Serial.printf("    import=%lu export=%lu ", (unsigned long)import, (unsigned long)export_);
  if (power == 0x7FFFFFFF) Serial.println("power=n/a");
  else                     Serial.printf("power=%ld\n", (long)power);
  return crcOk;
}

// ---- RX handling -----------------------------------------------------------
static void handleRx() {
  uint8_t buf[256];
  int len = radio.getPacketLength();
  int st  = radio.readData(buf, len);
  radio.startReceive();
  if (st != RADIOLIB_ERR_NONE || len < 4) return;

  led_blip();                        // brief LED flash on any received LoRa frame (activity indicator)
  float rssi = radio.getRSSI();
  uint8_t handle[3] = { buf[0], buf[1], buf[2] };

  if (isIgnoredHandle(handle)) return;   // phantom id FFFFFD — drop entirely: no radio log, no reader, no reply

  // XOR-decode the whole frame (byte3..end) with the handle key — recovers cmd
  // and, for control frames, the plaintext payload (energy payload stays TEA).
  uint8_t d[256]; memcpy(d, buf, len);
  obi_xor(d, len, handle, 3);
  uint8_t cmd = d[3] & 0x3F;

  gw_radio_log('R', handle, cmd, len, (int)rssi, "", d, len);   // live radio view (/radio) — d = XOR-decoded frame
  // Always mark a known reader "seen" on ANY frame it sends (even ones we can't decode / don't ack),
  // so the UI keeps it fresh whether or not it's assigned/keyed.
  { Reader *seen = findReader(handle);
    if (seen) { seen->lastSeenMs = millis(); seen->lastRssi = rssi; } }

  Serial.printf("\nRX %02X%02X%02X cmd=%u len=%d rssi=%.0f\n", handle[0], handle[1], handle[2], cmd, len, rssi);

  // The reader (bootloader AND main firmware) needs a few ms to turn its radio TX->RX after sending; answer
  // any request too early and it isn't listening yet. Wait here so EVERY response (bind/ECDH/ack/OTA block)
  // lands in the reader's RX window — same fix that made the reader OTA fast, applied to all replies.
  // (skip for a foreign beacon: we never reply to it, so no need to burn the delay.)
  if (cmd != OBI_CMD_TIME_BEACON) otaRespWait();

  switch (cmd) {
    // The reader keeps sending its "find gateway" frame between energy reports and needs the
    // matching reply EVERY time (even when already keyed) or it stalls in the announce phase.
    // Reply per-command; do NOT touch an existing key here (a reset shows up as failed decrypts).
    case OBI_CMD_READER_ANNOUNCE: {                         // 17 legacy announce -> cmd 49
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      storeAnnounce(r, cmd, d + 4, len - 4);                 // legacy announce carries the UUID too
      Serial.printf("  %02X%02X%02X announce(17)%s\n", handle[0], handle[1], handle[2],
                    acceptReader(r) ? " -> cmd49" : " (unassigned — greyed, not bound)");
      if (r->assigned) sendActivateOld(r); break;
    }
    case 18: {                                              // legacy reconnect -> cmd 50
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      Serial.printf("  %02X%02X%02X reconnect(18)\n", handle[0], handle[1], handle[2]);
      if (acceptReader(r)) sendReconnectAck(r, rssi); break;
    }
    case 35: {                                              // 1.2.x DevicesScan announce -> cmd 36 + bind
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      storeAnnounce(r, cmd, d + 4, len - 4);                 // grab the 16-byte UUID + type + version
      Serial.printf("  %02X%02X%02X announce(35)%s\n", handle[0], handle[1], handle[2],
                    acceptReader(r) ? " -> cmd36+bind" : " (unassigned — greyed, not bound)");
      if (r->assigned) { sendScanAck(r); sendBind(r); } break;
    }
    case 58: {                                              // 1.2.x reconnect -> bind
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      Serial.printf("  %02X%02X%02X reconnect(58)\n", handle[0], handle[1], handle[2]);
      if (acceptReader(r)) sendBind(r); break;
    }
    case OBI_CMD_ECDH: {                                    // reader's 64-byte pubkey (XOR-decoded in d[])
      Reader *r = addReader(handle);
      if (!r) break;
      if (!acceptReader(r)) { Serial.println("  ecdh from unassigned reader — ignoring"); break; }
      if (len - 4 < 64) { Serial.println("  ecdh: short pubkey"); break; }
      uint8_t secret[32];
      if (obi_ecdh_compute(d + 4, secret)) {
        memcpy(r->key, secret, 16);                         // TEA key = first 16 of shared X
        r->haveKey = true;
        Serial.print("  ECDH ok, TEA key = "); hexdump(r->key, 16); Serial.println();
        sendEcdhReply(r);                                   // send our pubkey back
      } else {
        Serial.println("  ECDH compute failed");
      }
      break;
    }
    case 33: {                                             // reader bootloader OTA pull (metadata/block)
      Reader *r = addReader(handle);                         // show a stuck-in-bootloader reader in the UI
      if (r) { r->lastRssi = rssi; r->lastSeenMs = millis(); r->inBootloader = true; }
      if (g_otaActive && !memcmp(handle, g_otaTarget, 3)) serveOtaRequest(handle, d + 4, len - 4);
      else Serial.printf("  cmd33 (OTA pull) but no armed image for %02X%02X%02X\n", handle[0], handle[1], handle[2]);
      break;
    }
    case 20: case 21: {                                    // LEGACY bootloader OTA pull (cmd 20/21)
      Reader *r = addReader(handle);
      if (r) { r->lastRssi = rssi; r->lastSeenMs = millis(); r->inBootloader = true; }
      if (g_otaActive && !memcmp(handle, g_otaTarget, 3)) serveOtaLegacy(handle, cmd, d + 4, len - 4);
      else Serial.printf("  cmd%u (OTA pull) but no armed image for %02X%02X%02X\n", cmd, handle[0], handle[1], handle[2]);
      break;
    }
    case OBI_CMD_ENERGY_U16: case OBI_CMD_ENERGY_U32: case OBI_CMD_ENERGY_RT:
    case 24: case 25:                                       // energy with trailing CRC16 (legacy reader)
    case OBI_CMD_ENERGY_D:   case OBI_CMD_ENERGY_LIVE_D: {
      Reader *r = addReader(handle);                        // learn its handle even from energy frames
      // ACK immediately — the reader is waiting for it and retries at ~5-6 s without it.
      // Normally advertise version 57 (no upgrade); when an OTA is armed for THIS reader, advertise
      // the new version so it resets after 3 acks and pulls the firmware.
      // Only ack ASSIGNED readers — an unassigned one is left to search (so another gateway can take it).
      if (r && acceptReader(r)) {
        // no-op version = 0, the UNIVERSAL no-op: every reader generation treats 0 (and its own softver)
        // as "no OTA available". Advertising a fixed 57 / the reader's softver is unsafe for a reader whose
        // no-op magic differs — e.g. the v38 meter reader treats only 0 or 38 as no-op, so a stray 57 (sent
        // in the window before we've decoded its softver, or by a plain default) looks like a real firmware
        // offer; after 3 such acks it arms its OTA marker and resets into the bootloader to pull an image
        // that isn't there, getting stuck in a cmd-33 loop (IR never reads -> no values). 0 avoids that for
        // all generations. The OTA target still gets the real new version to trigger a genuine update.
        uint8_t ver = (g_otaActive && !memcmp(r->handle, g_otaTarget, 3)) ? g_otaVersion : 0;
        if (cmd == 24 || cmd == 25) sendLegacyAck(r, cmd, ver, rssi);   // old v32 _c reader: cmd 56/57 TEA
        else                        sendEnergyAck(r, cmd, ver);          // 1.2.x _d reader: cmd 38/40 TEA
        // A keyless v3x reader (plaintext energy, never did ECDH) never gets the TEA ack above — both
        // ack builders bail on !haveKey. Drive it via the plaintext completedata ACK (cmd 51) instead:
        // carries the interval, and advertises the OTA version (0 = no-op) so an armed OTA still resets
        // the reader into the bootloader after 3 acks — no key needed.
        if (!r->haveKey && (cmd == OBI_CMD_ENERGY_U16 || cmd == OBI_CMD_ENERGY_U32 || cmd == OBI_CMD_ENERGY_RT)) {
          uint8_t rawver = (g_otaActive && !memcmp(r->handle, g_otaTarget, 3)) ? g_otaVersion : 0;
          sendCompletedataRaw(r, cmd | 0x20, rawver);    // ack cmd = energy_cmd | 0x20 (v24: 22->54)
        }
        // Just assigned a reader that is TEA-paired (has a key on its side from a previous gateway/session)
        // but we hold no key: nudge it to re-bind + re-ECDH right here in its RX window (the 3 s loop often
        // misses it). This is what turns "Add" into an actual re-pair on the air.
        if (!r->haveKey) sendPairAcks(r, rssi);
      }
      size_t plen = (len - 4) & ~0x7u;
      if (r && plen) {
        uint8_t p[256];
        const char *how = nullptr;  bool legacy = false;
        // The energy payload is always TEA-ECB with the per-device ECDH key — on the old v32 reader
        // (cloud "1.0.1") just as on 1.2.x (confirmed by pairing real hardware; the earlier "old readers
        // send a plaintext / 1-byte-XOR energy payload" assumption did NOT hold). So we only ever
        // TEA-decrypt: try the 1.2.x CRC layout first, then the legacy layout (no crc -> plausibility).
        if (r->haveKey) { memcpy(p, d + 4, plen); obi_tea_ecb_decrypt(p, plen, r->key);
                          if (energyValid(p, plen)) how = "tea/1.2.x"; }
        if (!how && r->haveKey) { memcpy(p, d + 4, plen); obi_tea_ecb_decrypt(p, plen, r->key);
                                  if (legacyValid(p, plen)) { how = "tea/legacy"; legacy = true; } }
        // Plaintext fallback — a reader that never completed the ECDH key exchange (e.g. an old firmware
        // freshly out of the bootloader) sends its energy UNENCRYPTED. ONLY for readers with no key, so a
        // paired reader's TEA ciphertext is never touched. The legacy layout has NO internal CRC — only a
        // loose plausibility check that WOULD false-match ciphertext — so restrict it to the legacy energy
        // commands (19/22/23); a 1.2.x reader (cmd 37/39) is never plaintext-legacy-decoded. The 1.2.x
        // layout carries its own CRC16, so that variant is safe to try on any command.
        bool legacyCmd = (cmd == OBI_CMD_ENERGY_U16 || cmd == OBI_CMD_ENERGY_U32 || cmd == OBI_CMD_ENERGY_RT);
        if (!how && !r->haveKey) { memcpy(p, d + 4, plen);
                                   if (energyValid(p, plen)) how = "plain/1.2.x"; }
        if (!how && !r->haveKey && legacyCmd) { memcpy(p, d + 4, plen);
                                   if (legacyValid(p, plen)) { how = "plain/legacy"; legacy = true; } }
        if (how) {
          Serial.printf("  energy [%s]: ", how); hexdump(p, plen); Serial.println();
          if (legacy) printEnergyOld(p, plen); else printEnergy(p, plen);
          r->decoded = true; r->decFails = 0; r->inBootloader = false;   // back to normal app operation
          // store telemetry for the dashboard / MQTT
          r->haveData = true; r->legacy = legacy; r->lastSeenMs = millis(); r->lastRssi = rssi;
          if (legacy) { r->softver = p[0]; r->hardver = p[1]; r->battery_mV = 20 * p[2]; r->flags = p[3];
                        r->import_ = rd_be32(p + 4); r->export_ = rd_be32(p + 8); r->power = rd_be32(p + 12); }
          else        { r->softver = p[2]; r->hardver = p[3]; r->battery_mV = 20 * p[4]; r->flags = p[5];
                        r->import_ = rd_be32(p + 6); r->export_ = rd_be32(p + 10); r->power = rd_be32(p + 14); }
        } else {
          Serial.print("  energy undecoded. raw: "); hexdump(d + 4, plen); Serial.println();
          // a keyed reader whose frames stop decrypting was reset -> drop the stale key and re-pair
          if (r->haveKey && ++r->decFails >= 3) {
            r->haveKey = false; r->decoded = false; r->decFails = 0;
            Serial.println("  (key stale — reader was reset; will re-pair on next announce)");
          }
        }
      }
      break;
    }
    case OBI_CMD_TIME_BEACON:
      Serial.println("  (another gateway's beacon — is the real Obi bridge still on?)");
      break;
    default:
      Serial.print("  payload(xor): "); hexdump(d + 4, len - 4); Serial.println();
      break;
  }
}

// ============ case button — factory reset (closed-case units) ============
// The original OBI gateway (OBI_BOARD_OBI_C3) is a sealed enclosure with a single button and a locked
// bootloader: once custom firmware is delivered over the cloud OTA path there is no UART/JTAG to fix a
// wrong WiFi/MQTT config. So the button is the escape hatch: hold it >= 5 s to wipe WiFi + MQTT + the
// stored reader UUIDs and reboot into the 'OpenOBI-<MAC>' captive portal. A short tap does nothing.
// Compiled only when the selected board defines PIN_BUTTON (see board_config.h).
#ifdef PIN_BUTTON
#ifndef OBI_RESET_HOLD_MS
#define OBI_RESET_HOLD_MS 5000
#endif

volatile bool g_btnArming = false;   // true while the reset button is held past the "arming" point

static inline bool buttonPressed() {
#if defined(BUTTON_ACTIVE_LOW) && BUTTON_ACTIVE_LOW
  return digitalRead(PIN_BUTTON) == LOW;
#else
  return digitalRead(PIN_BUTTON) == HIGH;
#endif
}

static void doFactoryReset() {
  Serial.println("\n[btn] factory reset — wiping WiFi + MQTT + reader list + assignments");
  g_uuidStore.begin("obiuuid", false); g_uuidStore.clear(); g_uuidStore.end();      // stored reader UUIDs
  { Preferences p; p.begin("obiassign", false); p.clear(); p.end(); }                       // reader assignments
  web_factory_reset();                                                          // WiFi + MQTT settings
  for (int i = 0; i < 20; i++) { led_write(i & 1); delay(100); }                // ~2 s blink = confirmed
  Serial.println("[btn] done — rebooting into setup portal");
  delay(200);
  ESP.restart();
}

static void buttonSetup() {
#if defined(BUTTON_ACTIVE_LOW) && BUTTON_ACTIVE_LOW
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#else
  pinMode(PIN_BUTTON, INPUT_PULLDOWN);
#endif
  Serial.printf("button: GPIO%d (hold %ds -> factory reset)\n", PIN_BUTTON, OBI_RESET_HOLD_MS / 1000);
}

static void buttonLoop() {
  static uint32_t pressStart = 0;
  static bool triggered = false;
  if (buttonPressed()) {
    uint32_t now = millis();
    if (!pressStart) { pressStart = now; triggered = false; }
    uint32_t held = now - pressStart;
    g_btnArming = held > 1000;                         // LED shows the "about to reset" fast blink
    if (!triggered && held >= OBI_RESET_HOLD_MS) { triggered = true; doFactoryReset(); }
  } else {
    pressStart = 0;
    g_btnArming = false;
  }
}
#endif // PIN_BUTTON

// Dedicated LoRa task: blocks on the DIO1 IRQ (via g_loraSem), so it wakes instantly to answer a reader
// and yields the core to web/MQTT when idle. High priority => on the single-core C3 it preempts the web
// task and hits the reader's tight OTA request→block window (fixes the slow reader OTA on the C3).
static void loraTask(void *) {
  static uint32_t lastBeacon = 0, lastScan = 0;
  for (;;) {
    xSemaphoreTake(g_loraSem, pdMS_TO_TICKS(20));   // wake on RX/TX IRQ, else tick every 20 ms for the beacon
    if (g_rx) { g_rx = false; handleRx(); }
    uint32_t now = millis();
    bool ota = gw_ota_active();
    if (now - lastBeacon >= 1000u) { lastBeacon = now; sendBeacon(); }   // keep 1 Hz beacon (readers pace to it)
    if (!ota && now - lastScan >= 3000) { lastScan = now; sendScan(); }
    if (!ota)                                         // re-pair safety net for assigned, not-yet-keyed readers
      for (auto &r : readers)
        if (r.used && r.assigned && !r.haveKey && now - r.lastBind >= 3000) sendPairAcks(&r, r.lastRssi);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  delay(200);
  flash_dbg_force_dio();   // OBI board: switch esp_flash to DIO before anything reads flash/NVS/OTA
  led_setup(); led_set(LED_BOOT);   // status LED (boards with PIN_STATUS_LED, e.g. OBI gateway GPIO0)
  Serial.println("\n=== OBI LoRa mini-gateway ===");
  Serial.printf("gwid: %.6s   radio: %.1f MHz BW%.0f SF%d CR4/%d +%ddBm\n",
                (const char *)GWID, OBI_FREQ_MHZ, OBI_BW_KHZ, OBI_SF, OBI_CR, OBI_TXPWR_DBM);

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  int st = radio.begin(OBI_FREQ_MHZ, OBI_BW_KHZ, OBI_SF, OBI_CR,
                       OBI_SYNCWORD, OBI_TXPWR_DBM, OBI_PREAMBLE, LORA_TCXO_V, false);
  if (st != RADIOLIB_ERR_NONE) { Serial.printf("radio.begin FAILED %d — halt\n", st); while (1) delay(1000); }
#if LORA_DIO2_RFSW
  radio.setDio2AsRfSwitch(true);
#endif
  radio.setCRC(2);
  g_loraSem = xSemaphoreCreateBinary();
  radio.setDio1Action(onDio1);

  g_ecdhReady = obi_ecdh_generate(g_ourPub);
  Serial.printf("ECDH keypair: %s\n", g_ecdhReady ? "ready" : "FAILED");
  Serial.printf("bind crc16(gwid)=0x%04X\n", obi_crc16(GWID, 6));

  radio.startReceive();
  Serial.println("listening + beaconing...");

#ifdef PIN_BUTTON
  buttonSetup();   // case button: hold to factory-reset (closed-case OBI_BOARD_OBI_C3)
#endif
  flash_dbg_uart_begin();   // UART flash console, runs alongside the log (type 'help')
  // LoRa in its own task, priority ABOVE the TCP/IP stack (18) + web task (1) so on the single-core C3 the
  // WiFi/HTTP/MQTT work can't delay a reader's OTA block response past its short RX window (this is the
  // real reason the C3 was slow vs the dual-core S3). Below the WiFi driver (23) so WiFi stays healthy.
  xTaskCreatePinnedToCore(loraTask, "lora", 8192, nullptr, 20, nullptr, CONFIG_ARDUINO_RUNNING_CORE);
  web_setup();     // WiFi config portal + web dashboard + MQTT (non-fatal if WiFi is unavailable)
}

// loop() is now just the low-priority housekeeping — the radio/LoRa lives in loraTask (high priority),
// so nothing here can delay a reader's OTA response. Yields the core to web/MQTT + LoRa.
void loop() {
  uint32_t now = millis();

#ifdef PIN_BUTTON
  buttonLoop();   // case button: poll for a >=5 s hold -> factory reset
#endif
  flash_dbg_uart_poll();   // service the UART flash console (non-blocking)

  // status LED: pick the steady pattern (reset-arming > OTA > WiFi > setup-portal), then service it
  static uint32_t ledTick = 0;
  if (now - ledTick >= 100) {
    ledTick = now;
    bool arming = false;
#ifdef PIN_BUTTON
    arming = g_btnArming;
#endif
    if      (arming)                led_set(LED_RESET_ARMED);
    else if (gw_ota_active())       led_set(LED_OTA);
    else if (web_wifi_connected())  led_set(LED_WIFI);
    else                            led_set(LED_PORTAL);
  }
  led_loop();

  // Prune stale phantom readers: an UNBOUND entry (never accepted onto this gateway) that hasn't been heard
  // from in READER_STALE_MS is dropped so the list/UI/MQTT don't fill with ghosts (bad-decode ids like
  // FFFFFD). A real, still-present reader simply reappears on its next transmission. Assigned readers are
  // kept regardless — a bound reader going quiet is a signal we want to keep showing, not hide.
  static uint32_t lastPrune = 0;
  if (now - lastPrune >= 10000) {
    lastPrune = now;
    for (auto &r : readers)
      if (r.used && !r.assigned && now - r.lastSeenMs >= READER_STALE_MS) {
        Serial.printf("[prune] drop stale unbound reader %02X%02X%02X (unseen %lus)\n",
                      r.handle[0], r.handle[1], r.handle[2], (unsigned long)((now - r.lastSeenMs) / 1000));
        r.haveKey = false; r.used = false;
      }
  }

  web_loop();     // service the web dashboard + MQTT (both non-blocking)
  delay(5);       // don't busy-spin — free the single core for the LoRa + web tasks
}
