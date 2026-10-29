#pragma once
#include "Arduino.h"
#include <map>
#include <vector>

using FakeStorage = std::map<std::string, std::map<std::string, std::vector<uint8_t>>>;
extern FakeStorage testStorage;
extern bool testStorageFail;

class Preferences {
  std::string space;
 public:
  bool begin(const char *name, bool) { space = name; return true; }
  void end() {}
  bool isKey(const char *key) { return testStorage[space].count(key); }
  size_t getBytesLength(const char *key) { return isKey(key) ? testStorage[space][key].size() : 0; }
  size_t getBytes(const char *key, void *out, size_t size) {
    if (!isKey(key)) return 0;
    const auto &data = testStorage[space][key];
    if (size < data.size()) return 0;
    if (!data.empty()) memcpy(out, data.data(), data.size());
    return data.size();
  }
  size_t putBytes(const char *key, const void *data, size_t size) {
    if (testStorageFail) return 0;
    const auto *bytes = static_cast<const uint8_t *>(data);
    testStorage[space][key] = std::vector<uint8_t>(bytes, bytes + size);
    return size;
  }
  size_t putULong64(const char *key, uint64_t value) { return putBytes(key, &value, sizeof value); }
  uint64_t getULong64(const char *key, uint64_t fallback) {
    uint64_t value;
    return getBytesLength(key) == sizeof value && getBytes(key, &value, sizeof value) == sizeof value
        ? value : fallback;
  }
  bool clear() {
    if (testStorageFail) return false;
    testStorage[space].clear(); return true;
  }
};
