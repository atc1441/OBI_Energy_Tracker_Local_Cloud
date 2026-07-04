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
const int MAX_READERS = 4;
Reader readers[MAX_READERS];

static uint8_t  g_ourPub[64];        // our static ECDH public key (generated once)
static bool     g_ecdhReady = false;

static Reader *findReader(const uint8_t h[3]) {
  for (auto &r : readers) if (r.used && !memcmp(r.handle, h, 3)) return &r;
  return nullptr;
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

static Reader *addReader(const uint8_t h[3]) {
  Reader *r = findReader(h);
  if (r) return r;
  for (auto &x : readers) if (!x.used) {
    x = Reader{}; x.used = true; memcpy(x.handle, h, 3);
    if (loadUuid(h, x.uuid)) x.haveUuid = true;     // restore a previously-seen UUID
    return &x;
  }
  return nullptr;
}

// ---- radio RX flag ---------------------------------------------------------
volatile bool g_rx = false;
void IRAM_ATTR onDio1() { g_rx = true; }

static void hexdump(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) { if (p[i] < 16) Serial.print('0'); Serial.print(p[i], HEX); Serial.print(' '); }
}

// transmit a frame, then go back to RX
static void txFrame(const uint8_t *buf, size_t len, const char *what) {
  int st = radio.transmit((uint8_t *)buf, len);
  radio.startReceive();
  g_rx = false;                 // discard the TxDone that also pulses DIO1
  Serial.printf("  TX %-6s len=%d -> %d\n", what, (int)len, st);
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
  b[1] = 0xFF;
  b[2] = (uint8_t)(rssi < 0 ? -rssi : rssi);
  uint16_t c = obi_crc16(b, 3);
  b[3] = c & 0xFF; b[4] = c >> 8;
  obi_tea_ecb_encrypt(b, 8, r->key);                 // reader TEA-decrypts (sub_A1E0) before the CRC check
  uint8_t cmd = (rxcmd == 25) ? 57 : 56;
  uint8_t f[16];
  size_t n = obi_build_frame(f, r->handle, cmd, b, sizeof(b));
  txFrame(f, n, "ack-v32");
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
    txFrame(f, n, "ota-blk");
    if (pos < g_otaSize) { uint32_t s = pos + 64 > g_otaSize ? g_otaSize : pos + 64; if (s > g_otaServed) g_otaServed = s; }
    if (g_otaServed >= g_otaSize) {                   // whole image served -> stop advertising the version
      g_otaActive = false;
      Serial.println("[ota] full image served — disarmed (reader will validate & flash)");
    }
  }
}

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
    txFrame(f, n, "ota-blk");
    if (pos < g_otaSize) { uint32_t s = pos + 64 > g_otaSize ? g_otaSize : pos + 64; if (s > g_otaServed) g_otaServed = s; }
    Serial.printf("[ota%u] block @%u  served %u/%u\n", reqCmd, pos, g_otaServed, g_otaSize);
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

  float rssi = radio.getRSSI();
  uint8_t handle[3] = { buf[0], buf[1], buf[2] };

  // XOR-decode the whole frame (byte3..end) with the handle key — recovers cmd
  // and, for control frames, the plaintext payload (energy payload stays TEA).
  uint8_t d[256]; memcpy(d, buf, len);
  obi_xor(d, len, handle, 3);
  uint8_t cmd = d[3] & 0x3F;

  Serial.printf("\nRX %02X%02X%02X cmd=%u len=%d rssi=%.0f\n", handle[0], handle[1], handle[2], cmd, len, rssi);

  switch (cmd) {
    // The reader keeps sending its "find gateway" frame between energy reports and needs the
    // matching reply EVERY time (even when already keyed) or it stalls in the announce phase.
    // Reply per-command; do NOT touch an existing key here (a reset shows up as failed decrypts).
    case OBI_CMD_READER_ANNOUNCE: {                         // 17 legacy announce -> cmd 49
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      storeAnnounce(r, cmd, d + 4, len - 4);                 // legacy announce carries the UUID too
      Serial.printf("  %02X%02X%02X announce(17) -> cmd49\n", handle[0], handle[1], handle[2]);
      sendActivateOld(r); break;
    }
    case 18: {                                              // legacy reconnect -> cmd 50
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      Serial.printf("  %02X%02X%02X reconnect(18) -> cmd50\n", handle[0], handle[1], handle[2]);
      sendReconnectAck(r, rssi); break;
    }
    case 35: {                                              // 1.2.x DevicesScan announce -> cmd 36 + bind
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      storeAnnounce(r, cmd, d + 4, len - 4);                 // grab the 16-byte UUID + type + version
      Serial.printf("  %02X%02X%02X announce(35) -> cmd36+bind\n", handle[0], handle[1], handle[2]);
      sendScanAck(r); sendBind(r); break;
    }
    case 58: {                                              // 1.2.x reconnect -> bind
      Reader *r = addReader(handle); if (!r) break; r->lastRssi = rssi;
      Serial.printf("  %02X%02X%02X reconnect(58) -> bind\n", handle[0], handle[1], handle[2]);
      sendBind(r); break;
    }
    case OBI_CMD_ECDH: {                                    // reader's 64-byte pubkey (XOR-decoded in d[])
      Reader *r = addReader(handle);
      if (!r) break;
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
      if (r) {
        // no-op version = the reader's own reported softver (so it's a no-op both before AND after an
        // OTA, whatever version the new image reports — avoids a reflash loop). OTA target gets the
        // new version to trigger the update.
        uint8_t cur = r->softver ? r->softver : 57;
        uint8_t ver = (g_otaActive && !memcmp(r->handle, g_otaTarget, 3)) ? g_otaVersion : cur;
        if (cmd == 24 || cmd == 25) sendLegacyAck(r, cmd, ver, rssi);   // old v32 _c reader: cmd 56/57 TEA
        else                        sendEnergyAck(r, cmd, ver);          // 1.2.x _d reader: cmd 38/40 TEA
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

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  delay(200);
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
  radio.setDio1Action(onDio1);

  g_ecdhReady = obi_ecdh_generate(g_ourPub);
  Serial.printf("ECDH keypair: %s\n", g_ecdhReady ? "ready" : "FAILED");
  Serial.printf("bind crc16(gwid)=0x%04X\n", obi_crc16(GWID, 6));

  radio.startReceive();
  Serial.println("listening + beaconing...");

  web_setup();     // WiFi config portal + web dashboard + MQTT (non-fatal if WiFi is unavailable)
}

void loop() {
  static uint32_t lastBeacon = 0, lastScan = 0;
  uint32_t now = millis();

  if (g_rx) { g_rx = false; handleRx(); }

  if (now - lastBeacon >= 1000) { lastBeacon = now; sendBeacon(); }
  if (now - lastScan   >= 3000) { lastScan   = now; sendScan(); }

  // periodic re-pair safety net for any reader we've heard but not yet keyed
  for (auto &r : readers)
    if (r.used && !r.haveKey && now - r.lastBind >= 3000) sendPairAcks(&r, r.lastRssi);

  web_loop();     // service the web dashboard + MQTT (both non-blocking)
}
