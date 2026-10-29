#pragma once

#include <WebServer.h>

template<size_t N>
void sendGzipHtml(WebServer &server, const uint8_t (&data)[N]) {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char *>(data), N);
}
