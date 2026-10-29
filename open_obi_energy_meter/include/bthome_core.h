#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bthome {

constexpr size_t SERVICE_SIZE = 16;
constexpr uint64_t COUNTER_LIMIT = uint64_t{1} << 32;
constexpr uint64_t COUNTER_BLOCK = 4096;

using Encrypt = bool (*)(const uint8_t *, const uint8_t *, const uint8_t *,
                        size_t, uint8_t *, uint8_t *);
using Reserve = bool (*)(uint64_t, void *);

bool parseHex(const char *text, uint8_t *out, size_t bytes);
bool powerValue(uint32_t raw, int32_t &scaled);
bool powerPacket(uint32_t raw, const uint8_t key[16], const uint8_t mac[6],
                 uint32_t counter, uint8_t out[SERVICE_SIZE], Encrypt encrypt);
bool sampleFresh(bool valid, uint32_t received, uint32_t now, uint32_t maxAge);

class Counter {
 public:
  explicit Counter(uint64_t persisted = 0) : next_(persisted), end_(persisted) {}
  bool take(uint32_t &value, Reserve reserve, void *context);

 private:
  uint64_t next_;
  uint64_t end_;
};

}  // namespace bthome
