// reader_status_native.cpp — exercise include/reader_status.h on the host (see test_reader_status.py).
// The assertions live in C++ so they run against the very code the firmware compiles.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "reader_status.h"

static void silent_limit() {
  // five missed reports, but never under 15 min and never over an hour
  assert(obi_silent_limit_s(1) == 900);        // a "Live" reader must not cry wolf after 5 s
  assert(obi_silent_limit_s(25) == 900);       // the default interval: still the floor
  assert(obi_silent_limit_s(180) == 900);
  assert(obi_silent_limit_s(181) == 905);      // from here the factor leads
  assert(obi_silent_limit_s(300) == 1500);
  assert(obi_silent_limit_s(720) == 3600);
  assert(obi_silent_limit_s(9999) == 3600);    // an absurd interval cannot hide a dead reader for hours
}

static void live_reader_verdicts() {
  const uint16_t IV = 60;                      // limit = 900 s
  // healthy
  assert(obi_reader_problems(5, IV, false, 3200, false) == 0);
  assert(!strcmp(obi_status_name(obi_reader_problems(5, IV, false, 3200, false)), "ok"));
  // silence, exactly at the boundary (the limit itself is still fine -- strictly greater warns)
  assert(obi_reader_problems(900, IV, false, 3200, false) == 0);
  assert(obi_reader_problems(901, IV, false, 3200, false) == OBI_PROB_NO_SIGNAL);
  assert(!strcmp(obi_status_name(OBI_PROB_NO_SIGNAL), "no_signal"));
  // battery: the reader's own flag, or the voltage backstop
  assert(obi_reader_problems(5, IV, true, 3200, false) == OBI_PROB_BATTERY_LOW);
  assert(obi_reader_problems(5, IV, false, 2600, false) == OBI_PROB_BATTERY_LOW);
  assert(obi_reader_problems(5, IV, false, 2601, false) == 0);
  assert(obi_reader_problems(5, IV, false, 0, false) == 0);   // 0 mV = never reported, not a flat battery
  assert(!strcmp(obi_status_name(OBI_PROB_BATTERY_LOW), "battery_low"));
  // bootloader
  assert(obi_reader_problems(5, IV, false, 3200, true) == OBI_PROB_BOOTLOADER);
  assert(!strcmp(obi_status_name(OBI_PROB_BOOTLOADER), "bootloader"));
  // several at once: the mask carries both, the name reports the one that matters more
  uint8_t both = obi_reader_problems(5000, IV, true, 2400, false);
  assert(both == (OBI_PROB_NO_SIGNAL | OBI_PROB_BATTERY_LOW));
  assert(!strcmp(obi_status_name(both), "no_signal"));
  uint8_t three = obi_reader_problems(5000, IV, true, 2400, true);
  assert(three == (OBI_PROB_NO_SIGNAL | OBI_PROB_BATTERY_LOW | OBI_PROB_BOOTLOADER));
  assert(!strcmp(obi_status_name(three), "no_signal"));
  assert(!strcmp(obi_status_name(OBI_PROB_BOOTLOADER | OBI_PROB_BATTERY_LOW), "bootloader"));
  // missing outranks everything
  assert(!strcmp(obi_status_name(OBI_PROB_MISSING | OBI_PROB_NO_SIGNAL), "missing"));
  assert(!strcmp(obi_status_name(0), "ok"));
}

static void missing_grace() {
  // Right after a restart nothing has reported yet -- that must not raise an alarm for every reader.
  assert(!obi_missing_overdue(0, 60));
  assert(!obi_missing_overdue(2, 1));
  assert(!obi_missing_overdue(900, 60));
  assert(obi_missing_overdue(901, 60));
  assert(!obi_missing_overdue(1200, 300));     // a 5-minute reader gets 25 min of grace
  assert(obi_missing_overdue(1501, 300));
  assert(obi_missing_overdue(3601, 9999));     // ...but never more than an hour
}

static void the_reported_case() {
  // The report behind the feature: a reader on the default interval whose battery went flat days ago.
  const uint16_t IV = 25;
  uint8_t p = obi_reader_problems(3 * 86400, IV, false, 2350, false);
  assert(p & OBI_PROB_NO_SIGNAL);
  assert(p & OBI_PROB_BATTERY_LOW);
  assert(!strcmp(obi_status_name(p), "no_signal"));
  // and the same reader after a gateway restart: no live slot, so it is reported as missing instead
  assert(obi_missing_overdue(2 * 3600, IV));
  assert(!strcmp(obi_status_name(OBI_PROB_MISSING), "missing"));
}

int main() {
  silent_limit();
  live_reader_verdicts();
  missing_grace();
  the_reported_case();
  printf("reader status: all invariants hold\n");
  return 0;
}
