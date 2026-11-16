// tariff_window.h — time-of-day windows for the per-reader tariff schemes, shared with the host tests
// (like energy_filter.h: the rules live in a header so tests/test_tariff.py exercises the very code the
// firmware compiles, not a re-implementation of it).
//
// Every window edge is MINUTES SINCE LOCAL MIDNIGHT, 0..1439. Full hours were enough for the original
// day/night tariff, but §14a EnWG Modul 3 windows are not: since the switch to 15-minute metering
// intervals grid operators define them on quarter hours (a real schedule from the field: HT 10:45-12:30
// and 19:00-22:00, NT 00:00-02:00). Minute resolution covers that and anything else a tariff sheet throws
// at us; the web UI offers 15-minute steps, the firmware does not care.
//
// Two conventions carried over unchanged from the hour-based fields these replace:
//   * start == end means the slot is UNUSED (never matches), and
//   * start > end wraps past midnight (22:00 -> 06:00), so "ends at midnight" is end == 0.
#pragma once
#include <stdint.h>

#define OBI_DAY_MINUTES 1440

// Is minute-of-day `m` inside the [s,e) window?
static inline bool obi_in_window(uint16_t s, uint16_t e, uint16_t m) {
  if (s == e) return false;                                  // degenerate window = slot unused
  return s < e ? (m >= s && m < e) : (m >= s || m < e);      // s > e wraps past midnight
}

// §14a Modul 3 zones. The numeric values are part of the MQTT/web contract (0/1/2) -- do not renumber.
enum ObiZone { OBI_ZONE_ST = 0, OBI_ZONE_HT = 1, OBI_ZONE_NT = 2 };

// Which zone minute-of-day `m` falls into, given up to 2 HT and 2 NT windows. HT is tested first, so a
// misconfigured overlap resolves to the (usually pricier) HT zone rather than silently favouring NT; ST is
// the fallback for "in neither window" and therefore needs no window of its own.
static inline uint8_t obi_zone_at(const uint16_t htS[2], const uint16_t htE[2],
                                  const uint16_t ntS[2], const uint16_t ntE[2], uint16_t m) {
  for (int i = 0; i < 2; i++) if (obi_in_window(htS[i], htE[i], m)) return (uint8_t)OBI_ZONE_HT;
  for (int i = 0; i < 2; i++) if (obi_in_window(ntS[i], ntE[i], m)) return (uint8_t)OBI_ZONE_NT;
  return (uint8_t)OBI_ZONE_ST;
}

// Parse one window edge as it arrives over HTTP (/api/reader/price) or out of the NVS config blob. Three
// accepted spellings, so every client that was written against the hour-only API keeps working:
//   "10:45"  HH:MM           -> 645   (the new, minute-precise form; "24:00" folds to 0 = midnight)
//   "19"     a bare 0..23    -> 1140  (legacy: whole hour, exactly what this field used to mean)
//   "645"    bare 24..1440   -> 645   (minutes since midnight; 1440 folds to 0)
// Anything unparseable, negative or past 1440 yields `fallback` (the caller passes the value currently
// stored, so a malformed field leaves the configuration alone instead of silently moving a window).
// Note the one deliberate ambiguity: a bare 0..23 is always read as an HOUR, never as minutes. Use the
// HH:MM form ("0:15") for an edge inside the first hour.
static inline uint16_t obi_parse_minute(const char *v, uint16_t fallback) {
  if (!v) return fallback;
  while (*v == ' ' || *v == '\t') v++;
  if (*v < '0' || *v > '9') return fallback;          // empty, sign, or junk -> keep what is stored
  long a = 0;
  while (*v >= '0' && *v <= '9') { a = a * 10 + (*v++ - '0'); if (a > 100000) return fallback; }
  long m;
  if (*v == ':') {
    v++;
    long b = 0;
    if (*v < '0' || *v > '9') return fallback;        // "10:" is not a time
    while (*v >= '0' && *v <= '9') { b = b * 10 + (*v++ - '0'); if (b > 100000) return fallback; }
    if (b > 59) return fallback;
    m = a * 60 + b;
  } else {
    m = (a <= 23) ? a * 60 : a;                       // bare 0..23 = whole hour (legacy spelling)
  }
  while (*v == ' ' || *v == '\t') v++;
  if (*v) return fallback;                            // trailing junk ("10:45x")
  if (m > OBI_DAY_MINUTES) return fallback;
  return (uint16_t)(m % OBI_DAY_MINUTES);             // 1440 ("24:00") == 0 == midnight
}

// Whole-hour approximation of a window, used in two places that still speak hours: the legacy fields of
// the stored config blob (so DOWNGRADING to a firmware from before minute windows finds something usable
// rather than nonsense) and the `ht_win`/`nt_win` arrays of /api/readers, kept for clients written against
// them. Rounds to the nearest hour, and never lets a real window collapse into start == end, which the
// reader of those fields would take for "unused".
static inline void obi_window_hours(uint16_t s, uint16_t e, uint8_t *hs, uint8_t *he) {
  if (s == e) { *hs = 0; *he = 0; return; }
  uint8_t a = (uint8_t)(((s + 30) / 60) % 24);
  uint8_t b = (uint8_t)(((e + 30) / 60) % 24);
  if (a == b) b = (uint8_t)((a + 1) % 24);
  *hs = a; *he = b;
}
