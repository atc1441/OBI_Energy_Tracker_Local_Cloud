// setup_ap.h — when the 'OpenOBI-<MAC>' setup AP may be up.
// Opt-in (Settings, NVS "aplim"): the AP opens after boot or a short case-button tap for OBI_SETUP_AP_MS,
// then stays off even if WiFi drops later, so an unattended gateway never advertises its dashboard.
// Exceptions: no WiFi saved yet (the AP is the only way in) and a client still connected (don't cut a
// running setup or upload).
#pragma once
#include <stdint.h>

#ifndef OBI_SETUP_AP_MS
#define OBI_SETUP_AP_MS 600000UL   // 10 min
#endif

struct SetupApWindow {
  uint32_t sinceMs = 0;
  bool open = true;   // boot opens the first window

  void start(uint32_t nowMs) { sinceMs = nowMs; open = true; }

  // Latches closed on expiry, so millis() wrap-around can't reopen it.
  bool allowed(uint32_t nowMs, bool wifiSaved, int stations) {
    if (open && nowMs - sinceMs >= OBI_SETUP_AP_MS) open = false;
    return open || !wifiSaved || stations > 0;
  }
};
