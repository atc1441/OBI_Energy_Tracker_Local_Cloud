#pragma once

#include <WebServer.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// The dashboard pages live in flash as gzip blobs (embed_web.py) and the gateway has no decompressor,
// so the compressed stream is the only thing it can hand out. RFC 9110 allows that unless the request
// explicitly refuses gzip: a MISSING Accept-Encoding means "any coding is acceptable", while
// "identity" or "gzip;q=0" means we must not send it. Those clients get a 406 saying so, rather than a
// body they would render as binary noise (which is what a plain `curl -H 'Accept-Encoding: identity'`
// used to get).

// q-value of `token` in an Accept-Encoding list, or -1 when the token isn't listed. Plain pointer scan
// over the header -- no String temporaries, so it costs nothing per request.
inline float acceptEncodingQuality(const char *list, const char *token) {
  if (!list) return -1.0f;
  const size_t len = strlen(token);
  while (*list) {
    while (*list == ',' || *list == ' ' || *list == '\t') ++list;
    const char *item = list;
    const char *end  = item;
    while (*end && *end != ',') ++end;
    list = end;
    if (!*item) break;
    bool same = true;
    for (size_t i = 0; i < len && same; ++i)
      same = tolower((unsigned char)item[i]) == tolower((unsigned char)token[i]);
    if (!same) continue;
    const char *p = item + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p == end) return 1.0f;                       // bare token -> q=1
    if (*p != ';') continue;                         // longer name ("gzipped"), keep looking
    for (const char *q = p; q + 1 < end; ++q)
      if (tolower((unsigned char)q[0]) == 'q' && q[1] == '=') return (float)atof(q + 2);
    return 1.0f;                                     // parameters but no q= -> still acceptable
  }
  return -1.0f;
}

inline bool clientAcceptsGzip(WebServer &server) {
  if (!server.hasHeader("Accept-Encoding")) return true;   // absent -> anything goes (RFC 9110)
  String list = server.header("Accept-Encoding");
  float q = acceptEncodingQuality(list.c_str(), "gzip");
  if (q < 0.0f) q = acceptEncodingQuality(list.c_str(), "*");
  return q > 0.0f;
}

template<size_t N>
void sendGzipHtml(WebServer &server, const uint8_t (&data)[N]) {
  server.sendHeader("Vary", "Accept-Encoding");
  if (!clientAcceptsGzip(server)) {
    server.send(406, "text/plain",
                "This page is stored gzip-compressed in flash and the gateway cannot decompress it.\n"
                "The request refused gzip via Accept-Encoding, so there is nothing to send.\n"
                "Use a browser, or 'curl --compressed'.\n");
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char *>(data), N);
}
