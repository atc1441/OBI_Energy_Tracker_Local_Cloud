// hist_ram_native.cpp — exercise include/hist_ram.h on the host (see test_hist_ram.py).
// The assertions live in C++ so they run against the very code the firmware compiles.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "hist_ram.h"

static const uint8_t A[3] = {0xe1, 0xc6, 0xc5};
static const uint8_t B[3] = {0xd7, 0x0c, 0x9a};

// Everything the firmware does with the ring on a read: walk it oldest-first, keep one reader's rows and
// format each as the CSV line the flash log would hold (see ramSamplesCsv() in gateway_web.cpp).
static std::string csvFor(const ObiRamHistory &h, const uint8_t id[3]) {
  std::string out;
  char line[80];
  for (uint16_t i = 0; i < h.count(); i++) {
    const ObiRamSample *s = h.at(i);
    if (!s || !h.isFor(*s, id)) continue;
    if (obi_ram_row(*s, line, sizeof line) > 0) out += line;
  }
  return out;
}

static void detached_is_inert() {
  ObiRamHistory h;                       // no storage attached: a gateway with history on flash
  assert(!h.active() && h.count() == 0);
  h.push(A, 1000, 10, 0, 5);             // must not touch memory it does not have
  assert(h.count() == 0 && h.at(0) == nullptr);
  assert(csvFor(h, A).empty());
  h.forget(A);                           // and must not crash either
}

static void fills_then_wraps() {
  ObiRamSample mem[4];
  ObiRamHistory h;
  h.attach(mem, 4);
  assert(h.active() && h.count() == 0 && h.at(0) == nullptr);
  for (uint32_t i = 1; i <= 3; i++) h.push(A, 1000 + i, 100 + i, 0, 7);
  assert(h.count() == 3);
  assert(h.at(0)->ep == 1001 && h.at(2)->ep == 1003);   // oldest first
  assert(h.at(3) == nullptr);
  for (uint32_t i = 4; i <= 9; i++) h.push(A, 1000 + i, 100 + i, 0, 7);
  assert(h.count() == 4);                                // capped at capacity
  assert(h.at(0)->ep == 1006 && h.at(3)->ep == 1009);    // the oldest four fell out
  // the window really slides by one per push, including across the buffer end
  h.push(A, 1010, 110, 0, 7);
  assert(h.at(0)->ep == 1007 && h.at(3)->ep == 1010);
  // re-attaching starts over without touching the old contents' validity
  h.attach(mem, 4);
  assert(h.count() == 0 && h.at(0) == nullptr);
}

static void keeps_readers_apart() {
  ObiRamSample mem[6];
  ObiRamHistory h;
  h.attach(mem, 6);
  h.push(A, 2001, 500, 1, 60);
  h.push(B, 2002, 900, 2, 70);
  h.push(A, 2003, 501, 1, 61);
  h.push(B, 2004, 901, 2, 71);
  assert(h.count() == 4);
  assert(h.countFor(A) == 2 && h.countFor(B) == 2);
  assert(csvFor(h, A) == "2001,500,1,60\n2003,501,1,61\n");
  assert(csvFor(h, B) == "2002,900,2,70\n2004,901,2,71\n");
  // clearing one reader's history must not touch the other's
  h.forget(A);
  assert(h.countFor(A) == 0 && h.countFor(B) == 2);
  assert(csvFor(h, A).empty());
  assert(csvFor(h, B) == "2002,900,2,70\n2004,901,2,71\n");
  // dropped slots keep their place in the ring until they age out, and new samples still land
  h.push(A, 2005, 502, 1, 62);
  assert(csvFor(h, A) == "2005,502,1,62\n");
}

static void forget_across_a_wrap() {
  ObiRamSample mem[4];
  ObiRamHistory h;
  h.attach(mem, 4);
  for (uint32_t i = 1; i <= 6; i++) h.push((i % 2) ? A : B, 3000 + i, i, 0, 10 + i);
  assert(h.count() == 4);                 // holds 3003..3006, i.e. the read start is not slot 0
  assert(h.at(0)->ep == 3003);
  h.forget(B);                            // the tombstone path must not corrupt the entries it skips
  assert(csvFor(h, A) == "3003,3,0,13\n3005,5,0,15\n");
  assert(csvFor(h, B).empty());
  assert(h.count() == 4);                 // nothing is compacted away
}

static void row_format_matches_the_flash_log() {
  char line[80];
  ObiRamSample s;
  s.ep = 1794783600u; s.imp = 606141; s.exp = 43384; s.pw = (uint32_t)(int32_t)91;
  s.h[0] = A[0]; s.h[1] = A[1]; s.h[2] = A[2];
  assert(obi_ram_row(s, line, sizeof line) > 0);
  assert(std::string(line) == "1794783600,606141,43384,91\n");
  s.pw = (uint32_t)(int32_t)-250;                       // feed-in: power is signed
  obi_ram_row(s, line, sizeof line);
  assert(std::string(line) == "1794783600,606141,43384,-250\n");
  s.pw = 0x7FFFFFFFu;                                   // obi_na(): empty field, exactly as on flash
  assert(obi_na(s.pw));
  obi_ram_row(s, line, sizeof line);
  assert(std::string(line) == "1794783600,606141,43384,\n");
}

int main() {
  detached_is_inert();
  fills_then_wraps();
  keeps_readers_apart();
  forget_across_a_wrap();
  row_format_matches_the_flash_log();
  printf("RAM history ring: all invariants hold\n");
  return 0;
}
