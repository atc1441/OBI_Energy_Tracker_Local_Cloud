#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <type_traits>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
  template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T value) : std::string(std::to_string(value)) {}
};

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
struct FakeSerial {
  template<typename... Args> void printf(const char *, Args...) {}
};
extern FakeSerial Serial;
