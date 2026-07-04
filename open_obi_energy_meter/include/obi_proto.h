// obi_proto.h — OBI/heyOBI LoRa protocol helpers (reversed from firmware 1.2.1).
// Frame:  [handle:3 plaintext][b3 = type<<6 | cmd][payload...]
//   control frames (beacon/scan/bind): b3..end are 1-byte XOR, key = (h0+h1+h2)&0xFF
//   energy payload (v32 & 1.2.x):       bytes 4.. are TEA-ECB with the per-device ECDH key
#pragma once
#include <Arduino.h>

// -------- LoRa command numbers (proto 0) --------
enum : uint8_t {
  OBI_CMD_INTERVAL_BEACON = 14,   // gw->dev  addressed: push upload_interval (+time)
  OBI_CMD_TIME_BEACON     = 15,   // gw->all  broadcast: time-sync, payload[0:6]=gateway id
  OBI_CMD_READER_ANNOUNCE = 17,   // dev->gw  reader announces (id + rssi)
  OBI_CMD_ENERGY_U16      = 19,
  OBI_CMD_ENERGY_U32      = 22,
  OBI_CMD_ENERGY_RT       = 23,
  OBI_CMD_ECDH            = 32,   // both dirs: 64-byte P-256 public key
  OBI_CMD_SCAN_REQ        = 36,   // gw->all  scan request -> reader sends ANNOUNCE
  OBI_CMD_ENERGY_D        = 37,   // dev->gw  meter_data_d (seen on 1.2.x)
  OBI_CMD_ENERGY_AUX      = 38,   // dev->gw  aux/ack
  OBI_CMD_ENERGY_LIVE_D   = 39,
  OBI_CMD_BIND            = 59,   // gw->dev  addressed: [crc16(gwid)][gwid:6] -> reader stores gateway id
};

// broadcast handle used by the time beacon / scan
static const uint8_t OBI_BROADCAST[3] = { 0xFF, 0xFF, 0xFB };

// -------- CRC-16/MODBUS (reader's split-table CRC, sub_6118) --------
// underlying poly 0xA001 (reflected 0x8005), init 0xFFFF. The reader compares
// (payload[0]<<8)|payload[1] against its own value, so on the wire we emit the
// standard result low-byte-first: p[0]=crc&0xFF, p[1]=crc>>8.
static inline uint16_t obi_crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

// -------- 1-byte XOR (control-frame obfuscation) --------
static inline uint8_t obi_xor_key(const uint8_t handle[3]) {
  return (uint8_t)(handle[0] + handle[1] + handle[2]);
}
// XOR bytes [from..len) in place with the handle-derived key
static inline void obi_xor(uint8_t *buf, size_t len, const uint8_t handle[3], size_t from = 3) {
  uint8_t k = obi_xor_key(handle);
  for (size_t i = from; i < len; i++) buf[i] ^= k;
}

// -------- TEA-ECB (energy payload on 1.2.x) --------
// Standard TEA: 16-byte key (first 16 of the 32-byte ECDH secret), 32 rounds, delta 0x9E3779B9.
// The gateway runs on RISC-V and loads both key and block 32-bit words LITTLE-ENDIAN
// (verified in sub_4202745C / lora_decrypt_frame), so we must too.
static inline uint32_t obi_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void obi_wle32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static inline void obi_tea_decrypt_block(uint8_t blk[8], const uint32_t k[4]) {
  uint32_t v0 = obi_le32(blk), v1 = obi_le32(blk + 4);
  const uint32_t delta = 0x9E3779B9;
  uint32_t sum = 0xC6EF3720;                          // delta * 32
  for (int i = 0; i < 32; i++) {
    v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
    v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
    sum -= delta;
  }
  obi_wle32(blk, v0); obi_wle32(blk + 4, v1);
}
static inline void obi_tea_ecb_decrypt(uint8_t *buf, size_t len, const uint8_t key[16]) {
  uint32_t k[4] = { obi_le32(key), obi_le32(key+4), obi_le32(key+8), obi_le32(key+12) };
  for (size_t off = 0; off + 8 <= len; off += 8) obi_tea_decrypt_block(buf + off, k);
}

// TEA-ECB encrypt (inverse of the above) — used to build the gateway->reader energy ACK, which the
// reader TEA-decrypts with the same ECDH key (reader sub_B594, key @0x2000340E).
static inline void obi_tea_encrypt_block(uint8_t blk[8], const uint32_t k[4]) {
  uint32_t v0 = obi_le32(blk), v1 = obi_le32(blk + 4);
  const uint32_t delta = 0x9E3779B9;
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += delta;
    v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
    v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
  }
  obi_wle32(blk, v0); obi_wle32(blk + 4, v1);
}
static inline void obi_tea_ecb_encrypt(uint8_t *buf, size_t len, const uint8_t key[16]) {
  uint32_t k[4] = { obi_le32(key), obi_le32(key+4), obi_le32(key+8), obi_le32(key+12) };
  for (size_t off = 0; off + 8 <= len; off += 8) obi_tea_encrypt_block(buf + off, k);
}

// -------- frame build: [handle][b3=type<<6|cmd][payload], XOR from b3 --------
// returns total length. type is 0 for all control frames.
static inline size_t obi_build_frame(uint8_t *out, const uint8_t handle[3], uint8_t cmd,
                                     const uint8_t *payload, size_t plen, uint8_t type = 0) {
  out[0] = handle[0]; out[1] = handle[1]; out[2] = handle[2];
  out[3] = (uint8_t)((type << 6) | (cmd & 0x3F));
  if (payload && plen) memcpy(out + 4, payload, plen);
  size_t len = 4 + plen;
  obi_xor(out, len, handle, 3);          // XOR b3..end with the handle key
  return len;
}
