#include "bthome_core.h"

#include <openssl/evp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

static bool encrypt(const uint8_t *key, const uint8_t *nonce,
                    const uint8_t *plain, size_t size, uint8_t *cipher, uint8_t *tag) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int n = 0;
  const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, 13, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, 4, nullptr) == 1 &&
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
      EVP_EncryptUpdate(ctx, nullptr, &n, nullptr, size) == 1 &&
      EVP_EncryptUpdate(ctx, cipher, &n, plain, size) == 1 &&
      EVP_EncryptFinal_ex(ctx, cipher + n, &n) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_GET_TAG, 4, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

static bool failCipher(const uint8_t *, const uint8_t *, const uint8_t *,
                       size_t, uint8_t *, uint8_t *) { return false; }

struct Store { uint64_t high = 0; bool fail = false; unsigned writes = 0; };
static bool reserve(uint64_t high, void *context) {
  Store &s = *static_cast<Store *>(context);
  if (s.fail) return false;
  s.high = high; ++s.writes;
  return true;
}

static void selftest() {
  uint8_t key[16], mac[6], nonce[13], cipher[6], tag[4];
  assert(bthome::parseHex("231d39c1d7cc1ab1aee224cd096db932", key, 16));
  assert(bthome::parseHex("5448e68f80a5", mac, 6));
  assert(bthome::parseHex("5448e68f80a5d2fc4133221100", nonce, 13));
  const uint8_t plain[] = {0x02, 0xCA, 0x09, 0x03, 0xBF, 0x13};
  const uint8_t expected[] = {0xE4, 0x45, 0xF3, 0xC9, 0x96, 0x2B};
  const uint8_t expectedTag[] = {0x6C, 0x7C, 0x45, 0x19};
  assert(encrypt(key, nonce, plain, 6, cipher, tag));
  assert(!memcmp(cipher, expected, 6) && !memcmp(tag, expectedTag, 4));

  uint8_t untouched[] = {0xAA, 0xBB};
  assert(!bthome::parseHex("001g", untouched, 2));
  assert(!bthome::parseHex("001", untouched, 2));
  assert(!bthome::parseHex(nullptr, untouched, 2));
  assert(untouched[0] == 0xAA && untouched[1] == 0xBB);
  assert(bthome::parseHex("aBcD", untouched, 2));
  assert(untouched[0] == 0xAB && untouched[1] == 0xCD);

  int32_t scaled;
  assert(bthome::powerValue(0, scaled) && scaled == 0);
  assert(bthome::powerValue(uint32_t(-321), scaled) && scaled == -32100);
  assert(bthome::powerValue(21474836, scaled) && scaled == 2147483600);
  assert(bthome::powerValue(uint32_t(-21474836), scaled) && scaled == -2147483600);
  for (uint32_t raw : {0x7FFFFFFFu, 0xCCCCCCCDu, 21474837u, uint32_t(-21474837)})
    assert(!bthome::powerValue(raw, scaled));
  uint8_t packet[bthome::SERVICE_SIZE];
  memset(packet, 0xFF, sizeof packet);
  assert(!bthome::powerPacket(100, key, mac, 0, packet, failCipher));
  for (auto b : packet) assert(b == 0);

  Store store;
  bthome::Counter counter;
  uint32_t value = 999;
  store.fail = true;
  assert(!counter.take(value, reserve, &store));
  assert(value == 999 && store.high == 0);
  store.fail = false;
  for (unsigned i = 0; i < 4096; ++i) {
    assert(counter.take(value, reserve, &store));
    assert(value == i && store.high == 4096);
  }
  assert(store.writes == 1);
  store.fail = true;
  assert(!counter.take(value, reserve, &store));
  store.fail = false;
  assert(counter.take(value, reserve, &store) && value == 4096);
  assert(store.high == 8192 && store.writes == 2);
  bthome::Counter rebooted(store.high);
  assert(rebooted.take(value, reserve, &store) && value == 8192);
  bthome::Counter brownout(store.high);
  assert(brownout.take(value, reserve, &store) && value == 12288);
  bthome::Counter last(bthome::COUNTER_LIMIT - 1);
  assert(last.take(value, reserve, &store) && value == UINT32_MAX);
  assert(store.high == bthome::COUNTER_LIMIT);
  assert(!last.take(value, reserve, &store));
  bthome::Counter exhausted(store.high);
  assert(!exhausted.take(value, reserve, &store));

  assert(bthome::sampleFresh(true, 100, 1099, 1000));
  assert(!bthome::sampleFresh(true, 100, 1100, 1000));
  assert(!bthome::sampleFresh(false, 100, 100, 1000));
  assert(bthome::sampleFresh(true, 0xFFFFFFF0, 0x10, 33));
  assert(!bthome::sampleFresh(true, 0xFFFFFFF0, 0x10, 32));
  assert(!bthome::sampleFresh(true, 0, 0, 0));
  puts("native checks passed");
}

int main(int argc, char **argv) {
  if (argc == 1) { selftest(); return 0; }
  if (argc != 5) return 2;
  const uint32_t raw = static_cast<uint32_t>(strtoll(argv[1], nullptr, 10));
  const uint32_t counter = static_cast<uint32_t>(strtoull(argv[2], nullptr, 10));
  uint8_t key[16], mac[6], out[bthome::SERVICE_SIZE];
  if (!bthome::parseHex(argv[3], key, 16) || !bthome::parseHex(argv[4], mac, 6)) return 2;
  if (!bthome::powerPacket(raw, key, mac, counter, out, encrypt)) return 1;
  for (uint8_t b : out) printf("%02x", b);
  puts("");
}
