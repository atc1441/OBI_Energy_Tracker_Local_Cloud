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

bool powerPacket(uint32_t raw, const uint8_t key[16], const uint8_t mac[6],
                 uint32_t counter, uint8_t out[SERVICE_SIZE], Encrypt encrypt) {
  memset(out, 0, SERVICE_SIZE);
  int32_t scaled;
  if (!encrypt || !powerValue(raw, scaled)) return false;
  uint8_t plain[5] = {0x5C};
  little32(plain + 1, static_cast<uint32_t>(scaled));
  uint8_t nonce[13];
  memcpy(nonce, mac, 6);
  nonce[6] = 0xD2; nonce[7] = 0xFC; nonce[8] = 0x41;
  little32(nonce + 9, counter);
  out[0] = 0xD2; out[1] = 0xFC; out[2] = 0x41;
  little32(out + 8, counter);
  if (!encrypt(key, nonce, plain, sizeof plain, out + 3, out + 12)) {
    memset(out, 0, SERVICE_SIZE);
    return false;
  }
  return true;
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
