#include "bthome_core.h"

#include <limits.h>
#include <string.h>

namespace bthome {

static int hexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool parseHex(const char *text, uint8_t *out, size_t bytes) {
  if (!text || strlen(text) != bytes * 2) return false;
  for (size_t i = 0; i < bytes * 2; ++i)
    if (hexDigit(text[i]) < 0) return false;
  for (size_t i = 0; i < bytes; ++i)
    out[i] = (hexDigit(text[2 * i]) << 4) | hexDigit(text[2 * i + 1]);
  return true;
}

static void little32(uint8_t *out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = value >> (8 * i);
}

bool powerValue(uint32_t raw, int32_t &scaled) {
  if (raw == 0x7FFFFFFF || raw == 0xCCCCCCCD) return false;
  int64_t watts = raw;
  if (raw & 0x80000000) watts -= int64_t{1} << 32;
  const int64_t centiwatts = watts * 100;
  if (centiwatts < INT32_MIN || centiwatts > INT32_MAX) return false;
  scaled = static_cast<int32_t>(centiwatts);
  return true;
}

static bool energyValid(uint32_t wh) { return wh != 0x7FFFFFFF && wh != 0xCCCCCCCD; }

size_t powerPacket(uint32_t raw, uint32_t importWh, uint32_t exportWh, const uint8_t key[16],
                   const uint8_t mac[6], uint32_t counter, uint8_t out[SERVICE_SIZE], Encrypt encrypt) {
  memset(out, 0, SERVICE_SIZE);
  int32_t scaled;
  if (!encrypt || !powerValue(raw, scaled)) return 0;
  uint8_t plain[15];
  size_t n = 0;
  if (energyValid(importWh) && energyValid(exportWh)) {   // 0x4D: uint32, 0.001 kWh = 1 Wh
    plain[n++] = 0x4D; little32(plain + n, importWh); n += 4;
    plain[n++] = 0x4D; little32(plain + n, exportWh); n += 4;
  }
  plain[n++] = 0x5C;
  little32(plain + n, static_cast<uint32_t>(scaled)); n += 4;
  uint8_t nonce[13];
  memcpy(nonce, mac, 6);
  nonce[6] = 0xD2; nonce[7] = 0xFC; nonce[8] = 0x41;
  little32(nonce + 9, counter);
  out[0] = 0xD2; out[1] = 0xFC; out[2] = 0x41;
  little32(out + 3 + n, counter);
  if (!encrypt(key, nonce, plain, n, out + 3, out + 7 + n)) {
    memset(out, 0, SERVICE_SIZE);
    return 0;
  }
  return n + 11;
}

bool sampleFresh(bool valid, uint32_t received, uint32_t now, uint32_t maxAge) {
  return valid && maxAge > 0 && uint32_t(now - received) < maxAge;
}

bool Counter::take(uint32_t &value, Reserve reserve, void *context) {
  if (next_ >= COUNTER_LIMIT) return false;
  if (next_ == end_) {
    uint64_t upper = next_ + COUNTER_BLOCK;
    if (upper > COUNTER_LIMIT) upper = COUNTER_LIMIT;
    if (!reserve || !reserve(upper, context)) return false;
    end_ = upper;
  }
  value = static_cast<uint32_t>(next_++);
  return true;
}

}  // namespace bthome
