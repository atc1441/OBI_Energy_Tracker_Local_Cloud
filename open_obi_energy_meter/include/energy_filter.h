// energy_filter.h — what counts as a believable reading from a reader's cumulative Wh counters.
//
// Pure arithmetic, no Arduino/ESP headers, so tests/energy_native.cpp can drive exactly the code the
// firmware runs (see tests/test_energy.py). Applied in handleRx() (main.cpp); reader.h includes this
// header because almost everything that touches a reading needs obi_na().
#pragma once

#include <stdint.h>

// Is an energy field "no reading"? 0x7FFFFFFF is our own n/a sentinel; 0xCCCCCCCD is a garbage value some
// meters return for import/export (looks like an uninitialized-register fill pattern) -- both mean "void",
// never a real Wh/W reading.
static inline bool obi_na(uint32_t v) { return v == 0x7FFFFFFF || v == 0xCCCCCCCD; }

// ---- energy VALUE sanity filter (see GitHub issue #51: multi-billion-W spikes in Import/Export/Power) ----
// Neither frame-validity check in main.cpp looks at the actual import/export/power bytes: energyValid()
// only proves the 1.2.x layout's own CRC16 matches (~1/65536 false-accept, good), but legacyValid() only
// checks hardver+battery (~11% false-accept on random bytes) and never touches the energy fields at all. A
// frame can pass either check while still carrying garbage import/export/power -- confirmed NOT a
// radio-level bit error (the SX1262's own hardware CRC, radio.setCRC(2), already rejects a
// corrupted-in-the-air packet before it ever reaches here -- see handleRx()'s `if (st !=
// RADIOLIB_ERR_NONE ...) return;`). The two remaining candidates are a reader that computed a bad value
// before encrypting it (the same bug class already fixed for DWSB20.2TH, see reader_firmware_mod/README.md,
// just on a different/older reader generation) or a hardware-CRC-clean frame TEA-decrypted with a
// momentarily stale key (e.g. a reader-side re-bind racing the gateway's own key update) -- either way the
// fields downstream are untrustworthy and neither existing check catches it. No real
// residential/small-commercial installation draws anywhere near this many watts continuously, so bound both
// the reported power and the implied average power from the Wh-counter deltas.
static const uint32_t OBI_MAX_PLAUSIBLE_W = 50000;

// True if going from oldWh (the reader's last ACCEPTED reading) to newWh over dtMs implies an average power
// within OBI_MAX_PLAUSIBLE_W in either direction. The counters are cumulative and should only crawl, never
// leap -- this catches a wild jump regardless of which of the two causes above produced it.
static inline bool plausibleEnergyStep(uint32_t oldWh, uint32_t newWh, uint32_t dtMs) {
  if (dtMs < 1000) dtMs = 1000;   // floor -- avoid dividing by a near-zero interval on a fast retry
  int64_t deltaWh = (int64_t)newWh - (int64_t)oldWh;
  int64_t avgW = deltaWh * 3600000LL / (int64_t)dtMs;
  return avgW >= -(int64_t)OBI_MAX_PLAUSIBLE_W && avgW <= (int64_t)OBI_MAX_PLAUSIBLE_W;
}

// ---- the stuck-at-zero reading (GitHub issue #99) ----------------------------------------------------
// A counter that reads exactly 0 while a real non-zero total is already on record is not a meter value:
// these registers only ever count up. Some readers emit a lone 0 for a frame or two, and a reader that
// cannot get a PIN from the meter reports nothing else to cross-check it against.
//
// The step filter above already rejects the enormous implied power of such a drop -- but on its own that is
// not enough. main.cpp deliberately adopts a *repeating* implausible value as a new baseline, so that a
// reader moved to another meter (or a meter that got replaced) is not stuck forever on an obsolete total.
// A reader sitting at 0 for several frames looks exactly like that, so the 0 gets adopted, and then the
// real total coming back reads as a jump of the entire meter reading: in the reporter's log, 12 474 kWh ->
// 0 -> 12 475 kWh, a 7-million-watt "average power", and the whole 12 474 kWh landing in that day's total.
//
// Refusing 0 outright costs a genuinely swapped meter nothing: a new meter passes through 0 only until it
// has registered its first watt-hour, and that first non-zero reading is adopted through the normal streak
// path minutes later.
static inline bool zeroedOutCounter(uint32_t oldWh, uint32_t newWh) {
  return newWh == 0 && oldWh != 0 && !obi_na(oldWh);
}
