// ota_slot_guard.h — "is the spare OTA slot free for the history store to take over?"
// -----------------------------------------------------------------------------------------------
// The raw-sample store lives in the currently inactive app partition (see ota_hist.h). On a gateway that
// was just converted from stock firmware, that slot still holds the ORIGINAL vendor image: the stock
// bridge was running from one slot and the cloud OTA delivered this firmware into the other one, so the
// image that was running a minute ago is sitting right there, untouched -- the only copy of it that
// exists anywhere, on hardware whose bootloader is fused shut (no UART readout, see firmware/README.md).
// Mounting the store over it formats it away, which is exactly what GitHub issue #100 reported: the
// documented "convert, then pull a full stock backup from the Debug page" route found a freshly
// formatted filesystem where the stock image used to be, a few minutes after the first boot.
//
// So before claiming the slot, look at what is physically in it and at what we know about it. Decided
// here, as plain byte/boolean predicates with no Arduino or IDF dependency, so the whole truth table can
// be pinned by tests/ota_slot_native.cpp on the host -- including the two cases that must NOT hold back
// (an existing install, and an install one self-update later), because getting those wrong would quietly
// shrink everyone's history to the 128 KB fallback instead.
#pragma once
#include <stdint.h>
#include <string.h>

// A littlefs volume starts each of its two superblocks with a 4-byte revision count and a 4-byte tag,
// followed by the literal "littlefs" -- so the magic sits at offset 8 of block 0 and again at offset 8 of
// block 1 (4 KB in). Verified against a live gateway (0x10000+8 and 0x11000+8 both read "littlefs") and
// against the flash dump attached to issue #100, which found the same magic at +0x8 and +0x1008.
static inline bool otaSlotLooksLikeLittlefs(const uint8_t *first16) {
  return first16 && memcmp(first16 + 8, "littlefs", 8) == 0;
}

// An ESP32 app image: the 0xE9 image magic in byte 0, and esp_app_desc_t's own magic word (0xABCD5432)
// at offset 0x20 -- directly after the 24-byte image header and the first 8-byte segment header. Both
// together, because 0xE9 alone also matches plenty of random flash content.
static const uint32_t OTA_SLOT_DESC_OFF = 0x20;
static inline bool otaSlotLooksLikeApp(const uint8_t *head, uint32_t len) {
  if (!head || len < OTA_SLOT_DESC_OFF + 4 || head[0] != 0xE9) return false;
  const uint8_t *d = head + OTA_SLOT_DESC_OFF;
  uint32_t magic = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
  return magic == 0xABCD5432UL;
}

// What is known about the spare slot when otaHist_mount() runs.
struct OtaSlotState {
  bool ourStore;   // it already holds this firmware's history filesystem (the everyday case)
  bool appImage;   // it holds a valid app image instead
  bool everUsed;   // this firmware has mounted a store there before, or has itself flashed that slot
  bool released;   // the user looked at the warning and chose the history capacity over the backup
};

// The one decision. Note what is NOT asked: whether the image in there is "the stock one". That question
// has no reliable answer -- an Arduino-built image carries the toolchain's own project name, not ours, so
// this firmware's old images and the vendor's are indistinguishable by their descriptors. "Have we ever
// put anything in that slot ourselves?" is answerable, persistent and precise: it is false exactly once,
// on the first boot after the conversion, which is exactly the boot that must keep its hands off.
static inline bool otaSlotClaimable(const OtaSlotState &s) {
  if (s.ourStore) return true;              // already ours -- mounting it loses nothing
  if (!s.appImage) return true;             // blank, or leftovers of an aborted flash: nothing to protect
  return s.everUsed || s.released;          // our own superseded firmware, or an explicit go-ahead
}

// Pull one NUL-padded text field out of an app descriptor into `out`, dropping anything unprintable, so a
// held-back image can be named in the UI ("EnergyTracking 1.0.1") instead of just sitting there as an
// address. Returns the length written. Fields are at fixed offsets inside esp_app_desc_t: version at
// +0x10, project_name at +0x30 (both 32 bytes).
static inline uint32_t otaSlotDescText(const uint8_t *head, uint32_t len, uint32_t fieldOff,
                                       char *out, uint32_t outSz) {
  if (!out || outSz == 0) return 0;
  out[0] = 0;
  uint32_t at = OTA_SLOT_DESC_OFF + fieldOff;
  if (!head || len < at + 32) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < 32 && n + 1 < outSz; i++) {
    uint8_t c = head[at + i];
    if (c == 0) break;                      // the field is NUL-padded; stop at the first terminator
    if (c < 0x20 || c > 0x7E) continue;     // never let raw flash bytes through into JSON or a page
    out[n++] = (char)c;
  }
  out[n] = 0;
  return n;
}
#define OTA_SLOT_DESC_VERSION 0x10
#define OTA_SLOT_DESC_PROJECT 0x30
