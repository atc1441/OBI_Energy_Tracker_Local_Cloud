// Host test for include/energy_filter.h — the production predicates, compiled as-is.
#include "energy_filter.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// The reporter's meter in GitHub issue #99: ~12 474 kWh of import, in Wh.
static const uint32_t TOTAL = 12474000;
static const uint32_t MINUTE = 60000;

static void zeroRule() {
  // The defect: a counter with a real total behind it drops to exactly 0.
  assert(zeroedOutCounter(TOTAL, 0));
  assert(zeroedOutCounter(1, 0));

  // Legitimate zeros stay legitimate. A meter that has never counted, and the very common case of a
  // house with no feed-in reporting export 0 forever, must not be filtered.
  assert(!zeroedOutCounter(0, 0));
  assert(!zeroedOutCounter(0, 5));

  // Coming out of a no-reading gap is not a drop either: there is no trustworthy total to protect.
  assert(!zeroedOutCounter(0x7FFFFFFF, 0));
  assert(!zeroedOutCounter(0xCCCCCCCD, 0));

  // This is what makes refusing 0 safe for a genuinely replaced meter: only exactly 0 is refused, so the
  // new meter's first registered watt-hour is free to become the new baseline.
  assert(!zeroedOutCounter(TOTAL, 1));
  assert(!zeroedOutCounter(TOTAL, 12475000));
}

static void stepRule() {
  // A normal crawl: 1 kWh over 40 minutes is 1.5 kW.
  assert(plausibleEnergyStep(TOTAL, TOTAL + 1000, 40 * MINUTE));
  // Standing still is always fine -- including 0 -> 0, which is precisely why the step filter alone could
  // not stop issue #99: once a 0 was on record, the next 0 looked perfectly healthy and the "new
  // baseline" streak adopted it. zeroedOutCounter() is what keeps the first 0 off the record.
  assert(plausibleEnergyStep(TOTAL, TOTAL, 40 * MINUTE));
  assert(plausibleEnergyStep(0, 0, 40 * MINUTE));

  // The drop itself, and the real total coming back, are both far outside the bound.
  assert(!plausibleEnergyStep(TOTAL, 0, 40 * MINUTE));
  assert(!plausibleEnergyStep(0, TOTAL, 40 * MINUTE));

  // The bound is OBI_MAX_PLAUSIBLE_W in both directions: 50 kW for an hour is 50 kWh.
  assert(plausibleEnergyStep(0, OBI_MAX_PLAUSIBLE_W, 60 * MINUTE));
  assert(!plausibleEnergyStep(0, OBI_MAX_PLAUSIBLE_W + 1, 60 * MINUTE));
  assert(plausibleEnergyStep(OBI_MAX_PLAUSIBLE_W, 0, 60 * MINUTE));
  assert(!plausibleEnergyStep(OBI_MAX_PLAUSIBLE_W + 1, 0, 60 * MINUTE));

  // dt is floored at 1 s, so a fast retry cannot divide its way to a huge implied power.
  assert(plausibleEnergyStep(0, 13, 0));      // 13 Wh in 1 s = 46.8 kW, just inside
  assert(!plausibleEnergyStep(0, 14, 0));
  assert(!plausibleEnergyStep(0, 14, 1));
}

static void sentinels() {
  assert(obi_na(0x7FFFFFFF));
  assert(obi_na(0xCCCCCCCD));
  assert(!obi_na(0));
  assert(!obi_na(TOTAL));
  assert(!obi_na(0xFFFFFFFF));
}

// Replay of the reporter's series through the accept/reject decision handleRx() makes. This mirrors that
// code by hand rather than calling it (it sits inside handleRx), so keep the two in step -- its worth is
// in pinning the SEQUENCE: before the fix, three readings of 0 in a row were adopted as a new baseline.
struct Meter {
  uint32_t total = 0;
  bool     haveData = false;
  uint32_t pending = 0;
  unsigned streak = 0;
  bool     havePending = false;

  // Returns true if the reading was stored.
  bool feed(uint32_t reading, uint32_t dtMs) {
    const bool zeroDrop = haveData && zeroedOutCounter(total, reading);
    bool plausible = !zeroDrop;
    if (!zeroDrop && haveData && !obi_na(total) && !obi_na(reading))
      plausible = plausibleEnergyStep(total, reading, dtMs);
    bool newBaseline = false;
    if (!plausible && !zeroDrop && !obi_na(reading)) {
      const bool matches = havePending && plausibleEnergyStep(pending, reading, dtMs);
      streak = matches ? streak + 1 : 1;
      havePending = true;
      pending = reading;
      if (streak >= 3) { newBaseline = true; havePending = false; streak = 0; }
    } else if (plausible) {
      havePending = false; streak = 0;
    }
    haveData = true;
    if (plausible || newBaseline) { total = reading; return true; }
    return false;
  }
};

static void replayIssue99() {
  Meter m;
  assert(m.feed(TOTAL, 0) && m.total == TOTAL);              // first reading, nothing to compare against
  assert(m.feed(TOTAL, 40 * MINUTE) && m.total == TOTAL);    // quiet meter, same total

  // Eight zeros in a row -- the run that used to clear the streak threshold and become the baseline.
  for (int i = 0; i < 8; i++) {
    assert(!m.feed(0, 5 * MINUTE));
    assert(m.total == TOTAL);                                // the last known total is still what we hold
  }

  // The real total comes back one watt-hour higher. Because 0 never landed, this is an ordinary crawl
  // rather than a 12 474 kWh "jump", so it is stored immediately and no day's total is inflated.
  assert(m.feed(TOTAL + 1000, 30 * MINUTE));
  assert(m.total == TOTAL + 1000);

  // A reader genuinely moved to another meter still gets through: an implausible but REPEATING non-zero
  // value is adopted after the third matching reading, exactly as before.
  Meter swapped;
  assert(swapped.feed(TOTAL, 0));
  assert(!swapped.feed(500, 40 * MINUTE));
  assert(!swapped.feed(501, 40 * MINUTE));
  assert(swapped.feed(502, 40 * MINUTE) && swapped.total == 502);

  // And a brand-new meter reading 0 is not stuck: it is refused only until it registers its first Wh.
  Meter fresh;
  assert(fresh.feed(TOTAL, 0));
  for (int i = 0; i < 5; i++) assert(!fresh.feed(0, 40 * MINUTE));
  assert(!fresh.feed(1, 40 * MINUTE));
  assert(!fresh.feed(2, 40 * MINUTE));
  assert(fresh.feed(3, 40 * MINUTE) && fresh.total == 3);
}

int main() {
  sentinels();
  zeroRule();
  stepRule();
  replayIssue99();
  puts("energy filter: all invariants hold");
  return 0;
}
