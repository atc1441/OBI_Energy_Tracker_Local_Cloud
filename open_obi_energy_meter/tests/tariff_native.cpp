// tariff_native.cpp — exercise include/tariff_window.h on the host (see test_tariff.py).
// The assertions live in C++ so they run against the very code the firmware compiles.
#include <cassert>
#include <cstdio>
#include <cstring>

#include "tariff_window.h"

static uint16_t hm(int h, int m) { return (uint16_t)(h * 60 + m); }

static void windows_basics() {
  // [start,end): start included, end excluded
  assert(!obi_in_window(hm(10, 45), hm(12, 30), hm(10, 44)));
  assert(obi_in_window(hm(10, 45), hm(12, 30), hm(10, 45)));
  assert(obi_in_window(hm(10, 45), hm(12, 30), hm(12, 29)));
  assert(!obi_in_window(hm(10, 45), hm(12, 30), hm(12, 30)));
  // a degenerate window is an unused slot, never a 24 h window
  for (int m = 0; m < OBI_DAY_MINUTES; m++) assert(!obi_in_window(0, 0, (uint16_t)m));
  for (int m = 0; m < OBI_DAY_MINUTES; m++) assert(!obi_in_window(hm(7, 15), hm(7, 15), (uint16_t)m));
  // start > end wraps past midnight; end == 0 means "through midnight"
  assert(obi_in_window(hm(22, 0), hm(6, 0), hm(23, 59)));
  assert(obi_in_window(hm(22, 0), hm(6, 0), hm(0, 0)));
  assert(obi_in_window(hm(22, 0), hm(6, 0), hm(5, 59)));
  assert(!obi_in_window(hm(22, 0), hm(6, 0), hm(6, 0)));
  assert(obi_in_window(hm(19, 0), 0, hm(23, 59)));
  assert(!obi_in_window(hm(19, 0), 0, hm(18, 59)));
  // quarter-hour edges are honoured to the minute, which is the whole point of issue #116
  assert(obi_in_window(hm(0, 15), hm(0, 30), hm(0, 15)));
  assert(!obi_in_window(hm(0, 15), hm(0, 30), hm(0, 30)));
}

static void zones_reporter_schedule() {
  // The schedule from the feature request: NT 00:00-02:00, HT 10:45-12:30 and 19:00-22:00, ST otherwise.
  const uint16_t htS[2] = {hm(10, 45), hm(19, 0)}, htE[2] = {hm(12, 30), hm(22, 0)};
  const uint16_t ntS[2] = {hm(0, 0)},  ntE[2] = {hm(2, 0)};
  struct Case { int h, m; uint8_t zone; };
  const Case cases[] = {
    {0, 0,  OBI_ZONE_NT}, {1, 59, OBI_ZONE_NT}, {2, 0,  OBI_ZONE_ST},
    {10, 44, OBI_ZONE_ST},                      // used to be billed as ST *and* still is -- before the fix
    {10, 45, OBI_ZONE_HT},                      // ...while this quarter hour was ST and is now HT
    {11, 0,  OBI_ZONE_HT}, {12, 29, OBI_ZONE_HT},
    {12, 30, OBI_ZONE_ST},                      // the rounded-to-13:00 workaround billed this as HT
    {18, 59, OBI_ZONE_ST}, {19, 0,  OBI_ZONE_HT}, {21, 59, OBI_ZONE_HT}, {22, 0,  OBI_ZONE_ST},
    {23, 59, OBI_ZONE_ST},
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    uint8_t z = obi_zone_at(htS, htE, ntS, ntE, hm(cases[i].h, cases[i].m));
    if (z != cases[i].zone) {
      printf("zone mismatch at %02d:%02d: got %u want %u\n", cases[i].h, cases[i].m, z, cases[i].zone);
      assert(false);
    }
  }
  // every minute of the day resolves to exactly one zone, and the three cover the day completely
  int n[3] = {0, 0, 0};
  for (int m = 0; m < OBI_DAY_MINUTES; m++) n[obi_zone_at(htS, htE, ntS, ntE, (uint16_t)m)]++;
  assert(n[OBI_ZONE_HT] == 105 + 180);            // 10:45-12:30 = 105 min, 19:00-22:00 = 180 min
  assert(n[OBI_ZONE_NT] == 120);                  // 00:00-02:00
  assert(n[OBI_ZONE_ST] == OBI_DAY_MINUTES - 105 - 180 - 120);
  // HT wins a misconfigured overlap; no window at all means ST all day
  const uint16_t oS[2] = {hm(9, 0)}, oE[2] = {hm(10, 0)};
  assert(obi_zone_at(oS, oE, oS, oE, hm(9, 30)) == OBI_ZONE_HT);
  const uint16_t zero[2] = {0, 0};
  for (int m = 0; m < OBI_DAY_MINUTES; m++) assert(obi_zone_at(zero, zero, zero, zero, (uint16_t)m) == OBI_ZONE_ST);
}

static void wire_format() {
  const uint16_t KEEP = 777;                      // sentinel: "field absent/invalid, keep what is stored"
  assert(obi_parse_minute("10:45", KEEP) == hm(10, 45));
  assert(obi_parse_minute("00:00", KEEP) == 0);
  assert(obi_parse_minute("0:15", KEEP) == 15);
  assert(obi_parse_minute("23:59", KEEP) == hm(23, 59));
  assert(obi_parse_minute("24:00", KEEP) == 0);   // ...folds to midnight, like end == 0
  assert(obi_parse_minute(" 7:30 ", KEEP) == hm(7, 30));
  // legacy spelling: a bare 0..23 is the whole hour it always meant
  assert(obi_parse_minute("0", KEEP) == 0);
  assert(obi_parse_minute("6", KEEP) == hm(6, 0));
  assert(obi_parse_minute("19", KEEP) == hm(19, 0));
  assert(obi_parse_minute("23", KEEP) == hm(23, 0));
  // bare minutes-since-midnight past the hour range
  assert(obi_parse_minute("24", KEEP) == 24);
  assert(obi_parse_minute("645", KEEP) == hm(10, 45));
  assert(obi_parse_minute("1439", KEEP) == hm(23, 59));
  assert(obi_parse_minute("1440", KEEP) == 0);
  // anything malformed leaves the stored value alone instead of moving a window to midnight
  const char *junk[] = {"", " ", "-1", "abc", "10:", "10:60", "10:xx", "1441", "25:00", "10:45x",
                        "99999999", ":30", "+5"};
  for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
    uint16_t got = obi_parse_minute(junk[i], KEEP);
    if (got != KEEP) { printf("junk %-10s accepted as %u\n", junk[i], got); assert(false); }
  }
  assert(obi_parse_minute(nullptr, KEEP) == KEEP);
}

static void hour_approximation() {
  uint8_t s = 9, e = 9;
  obi_window_hours(0, 0, &s, &e); assert(s == 0 && e == 0);                 // unused stays unused
  obi_window_hours(hm(19, 0), hm(22, 0), &s, &e); assert(s == 19 && e == 22);
  obi_window_hours(hm(10, 45), hm(12, 30), &s, &e); assert(s == 11 && e == 13);   // nearest hour
  obi_window_hours(hm(10, 20), hm(10, 40), &s, &e); assert(s == 10 && e == 11);   // never collapses to unused
  obi_window_hours(hm(23, 45), hm(0, 30), &s, &e); assert(s == 0 && e == 1);      // wraps/rounds past midnight
  obi_window_hours(hm(23, 50), hm(23, 55), &s, &e); assert(s == 0 && e == 1);
  // round trip of a whole-hour config: hours -> minutes -> hours is lossless
  for (int a = 0; a < 24; a++)
    for (int b = 0; b < 24; b++) {
      if (a == b) continue;
      obi_window_hours((uint16_t)(a * 60), (uint16_t)(b * 60), &s, &e);
      assert(s == a && e == b);
    }
}

int main() {
  windows_basics();
  zones_reporter_schedule();
  wire_format();
  hour_approximation();
  printf("tariff windows: all invariants hold\n");
  return 0;
}
