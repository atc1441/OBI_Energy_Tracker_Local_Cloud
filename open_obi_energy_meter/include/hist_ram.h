// hist_ram.h — the in-RAM sample ring the History page falls back to when raw samples are no longer
// written to flash (see g_histMode in gateway_web.cpp). Motivation: a setup that already archives every
// reading elsewhere — MQTT into InfluxDB, for example — wants the gateway to stop writing to flash
// altogether, but the History page should not go blank because of it.
//
// Pure logic, no Arduino types, so tests/test_hist_ram.py exercises the very code the firmware compiles
// (same arrangement as energy_filter.h / tariff_window.h).
#pragma once
#include <stdint.h>
#include <stdio.h>

#include "energy_filter.h"   // obi_na() -- a void power reading becomes an empty CSV field, as on flash

struct ObiRamSample {
  uint32_t ep;          // unix epoch of the sample; 0 marks a dropped entry (see forget())
  uint32_t imp, exp;    // Wh counters
  uint32_t pw;          // power -- signed once cast to int32_t, obi_na() when not available
  uint8_t  h[3];        // reader handle, so a single ring serves every reader
};

// Fixed-capacity ring over caller-owned storage: whoever switches the storage mode allocates the buffer,
// so a gateway that keeps its history on flash pays no heap for this at all, and the ring itself never
// allocates or frees anything.
struct ObiRamHistory {
  ObiRamSample *buf = nullptr;
  uint16_t cap = 0;     // entries the buffer can hold
  uint16_t head = 0;    // next slot to write
  uint16_t used = 0;    // entries written so far (<= cap)

  void attach(ObiRamSample *storage, uint16_t capacity) {
    buf = storage; cap = capacity; head = 0; used = 0;
  }
  void detach() { buf = nullptr; cap = 0; head = 0; used = 0; }
  bool active() const { return buf != nullptr && cap != 0; }
  uint16_t count() const { return used; }

  void push(const uint8_t handle[3], uint32_t ep, uint32_t imp, uint32_t exp, uint32_t pw) {
    if (!active()) return;
    ObiRamSample &s = buf[head];
    s.ep = ep; s.imp = imp; s.exp = exp; s.pw = pw;
    s.h[0] = handle[0]; s.h[1] = handle[1]; s.h[2] = handle[2];
    head = (uint16_t)((head + 1u) % cap);
    if (used < cap) used++;
  }

  // Oldest-first access (i = 0 is the oldest entry still held); nullptr past the end.
  const ObiRamSample *at(uint16_t i) const {
    if (!active() || i >= used) return nullptr;
    uint16_t start = (uint16_t)((head + cap - used) % cap);
    return &buf[(start + i) % cap];
  }

  bool isFor(const ObiRamSample &s, const uint8_t handle[3]) const {
    return s.ep != 0 && s.h[0] == handle[0] && s.h[1] == handle[1] && s.h[2] == handle[2];
  }

  uint16_t countFor(const uint8_t handle[3]) const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < used; i++) { const ObiRamSample *s = at(i); if (s && isFor(*s, handle)) n++; }
    return n;
  }

  // Drop one reader's samples (its log was cleared from the History page). Tombstoned in place with
  // ep = 0 rather than compacted: rewriting a ring while reading it would overwrite entries that have
  // not been read yet, and the dead slots are reused by the ring within one lap anyway.
  void forget(const uint8_t handle[3]) {
    for (uint16_t i = 0; i < used; i++) {
      const ObiRamSample *s = at(i);
      if (s && isFor(*s, handle)) const_cast<ObiRamSample *>(s)->ep = 0;
    }
  }
};

// One sample as the exact CSV line the flash log stores -- "ep,imp,exp,pw\n", with an empty power field
// when the reading was not available -- so the History page's parser never needs to know which of the two
// a row came from. Returns what snprintf() returns (bytes that would have been written).
static inline int obi_ram_row(const ObiRamSample &s, char *out, size_t n) {
  if (obi_na(s.pw))
    return snprintf(out, n, "%lu,%lu,%lu,\n",
                    (unsigned long)s.ep, (unsigned long)s.imp, (unsigned long)s.exp);
  return snprintf(out, n, "%lu,%lu,%lu,%ld\n",
                  (unsigned long)s.ep, (unsigned long)s.imp, (unsigned long)s.exp, (long)(int32_t)s.pw);
}
