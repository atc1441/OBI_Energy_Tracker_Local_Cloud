#pragma once
#include "Arduino.h"
#include <vector>
#include <utility>

class WebServer {
 public:
  std::vector<std::pair<String, String>> params;
  int code = 0;
  String body;
  int args() const { return params.size(); }
  String argName(int i) const { return params.at(i).first; }
  bool hasArg(const char *name) const {
    for (const auto &p : params) if (p.first == name) return true;
    return false;
  }
  String arg(const char *name) const {
    for (const auto &p : params) if (p.first == name) return p.second;
    return "";
  }
  void send(int status, const char *, const String &data) { code = status; body = data; }
  void sendHeader(const char *, const char *) {}
};
