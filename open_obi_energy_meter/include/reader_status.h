// reader_status.h — one definition of "is this reader healthy", shared by the dashboard's alert banner,
// the HTTP API, MQTT/Home Assistant and the host tests (same arrangement as energy_filter.h).
//
// The point of this file is that there is exactly ONE place deciding when a reader counts as broken. It
// answers the question a human asks ("should someone be told?"), which is deliberately slower and calmer
// than the per-reader MQTT availability flag in gateway_web.cpp (READER_AVAIL_*: 4 missed reports, floor
// 2 min) -- that one answers "is this value still current?" and may flap on a weak link without anyone
// needing to act.
#pragma once
#include <stdint.h>

// A reader can have several problems at once (silent AND nearly flat), so the dashboard is given the whole
// set as a bit mask while MQTT publishes the single worst one -- see obi_status_name().
enum {
  OBI_PROB_NO_SIGNAL   = 1 << 0,   // nothing heard from it for obi_silent_limit_s()
  OBI_PROB_BATTERY_LOW = 1 << 1,   // its own low-power flag, or below OBI_BATTERY_LOW_MV
  OBI_PROB_BOOTLOADER  = 1 << 2,   // sitting in its OTA bootloader: not metering anything
  OBI_PROB_MISSING     = 1 << 3,   // bound in NVS but has not reported at all since the gateway booted
};

#define OBI_SILENT_MISSES   5      // missed reports before a reader is called silent...
#define OBI_SILENT_MIN_S    900    // ...but never before 15 min, so a "Live" (1 s) reader cannot cry wolf
#define OBI_SILENT_MAX_S    3600   // ...and never later than an hour, so an absurd interval cannot hide a
                                    // dead reader for half a day
#define OBI_BATTERY_LOW_MV  2600   // backstop for readers that never raise their own low-power flag

// How long a reader on this interval may stay quiet before something is wrong.
static inline uint32_t obi_silent_limit_s(uint16_t interval_s) {
  uint32_t lim = (uint32_t)interval_s * OBI_SILENT_MISSES;
  if (lim < OBI_SILENT_MIN_S) lim = OBI_SILENT_MIN_S;
  if (lim > OBI_SILENT_MAX_S) lim = OBI_SILENT_MAX_S;
  return lim;
}

// Everything wrong with a reader that IS in the live list. battery_mV == 0 means "not reported yet", which
// is not a low battery.
static inline uint8_t obi_reader_problems(uint32_t age_s, uint16_t interval_s, bool lowpower,
                                           uint16_t battery_mV, bool bootloader) {
  uint8_t p = 0;
  if (age_s > obi_silent_limit_s(interval_s)) p |= OBI_PROB_NO_SIGNAL;
  if (lowpower || (battery_mV > 0 && battery_mV <= OBI_BATTERY_LOW_MV)) p |= OBI_PROB_BATTERY_LOW;
  if (bootloader) p |= OBI_PROB_BOOTLOADER;
  return p;
}

// A reader that is bound but has no live slot at all (it has not transmitted since boot) only counts as
// missing once a report from it is genuinely overdue -- right after a restart NOTHING has reported yet, and
// the gateway restarts on its own for every auto-update, so without this every reboot would raise an alarm
// for every reader in the house.
static inline bool obi_missing_overdue(uint32_t uptime_s, uint16_t interval_s) {
  return uptime_s > obi_silent_limit_s(interval_s);
}

// The single worst problem, as the short machine-readable word published over MQTT and shown in the API.
// Order = how much it matters: a reader we cannot reach at all outranks one that is merely low on battery.
static inline const char *obi_status_name(uint8_t problems) {
  if (problems & OBI_PROB_MISSING)     return "missing";
  if (problems & OBI_PROB_NO_SIGNAL)   return "no_signal";
  if (problems & OBI_PROB_BOOTLOADER)  return "bootloader";
  if (problems & OBI_PROB_BATTERY_LOW) return "battery_low";
  return "ok";
}
