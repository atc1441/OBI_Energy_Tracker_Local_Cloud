// Host self-test for include/ota_slot_guard.h -- the rules that decide whether the history store may
// format the spare OTA app slot (GitHub issue #100). Driven by tests/test_ota_slot.py.
//
// The byte fixtures below are not invented: HEAD_APP and HEAD_LFS were read straight off a running
// obi_gateway_c3 over /api/flash/read (the active app partition at 0x1F0000 and the mounted history store
// at 0x10000), and HEAD_OTHER is the head of that device's coredump partition, standing in for "a region
// that is neither". The whole point of the guard is which of these it mistakes for which.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_slot_guard.h"

// obi_gateway_c3 @0x1F0000 -- the real app image this firmware boots from.
static const uint8_t HEAD_APP[0x80] = {
  0xE9,0x05,0x02,0x2F,0xD0,0x22,0x38,0x40,0xEE,0x00,0x00,0x00,0x05,0x00,0x00,0x00,
  0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x01,0x20,0x00,0x12,0x3C,0xB0,0xE2,0x05,0x00,
  0x32,0x54,0xCD,0xAB,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x65,0x73,0x70,0x2D,0x69,0x64,0x66,0x3A,0x20,0x76,0x34,0x2E,0x34,0x2E,0x37,0x20,
  0x33,0x38,0x65,0x65,0x62,0x61,0x32,0x31,0x33,0x61,0x00,0x00,0x00,0x00,0x00,0x00,
  0x61,0x72,0x64,0x75,0x69,0x6E,0x6F,0x2D,0x6C,0x69,0x62,0x2D,0x62,0x75,0x69,0x6C,
  0x64,0x65,0x72,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x31,0x32,0x3A,0x32,0x39,0x3A,0x32,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
// obi_gateway_c3 @0x10000 -- the first bytes of the mounted raw-sample store.
static const uint8_t HEAD_LFS[16] = {
  0xD7,0x72,0x65,0x70,0xF0,0x0F,0xFF,0xF7,0x6C,0x69,0x74,0x74,0x6C,0x65,0x66,0x73,
};
// obi_gateway_c3 @0x3D0000 (coredump) -- neither of the two.
static const uint8_t HEAD_OTHER[0x80] = {
  0x44,0x36,0x00,0x00,0x02,0x01,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x7F,0x45,0x4C,0x46,0x01,0x01,0x01,0x00,
};

static int fail = 0;
static void check(const char *what, bool ok) {
  printf("    %-62s %s\n", what, ok ? "ok" : "** FAILED **");
  if (!ok) fail = 1;
}

static void contentProbes() {
  printf("  what the first bytes of the slot say (real flash reads from a live gateway)\n");
  check("the store's own superblock is recognized", otaSlotLooksLikeLittlefs(HEAD_LFS));
  check("an app image is not mistaken for the store", !otaSlotLooksLikeLittlefs(HEAD_APP));
  check("neither is a coredump partition", !otaSlotLooksLikeLittlefs(HEAD_OTHER));
  check("the real app image is recognized", otaSlotLooksLikeApp(HEAD_APP, sizeof HEAD_APP));
  check("the store is not mistaken for an app", !otaSlotLooksLikeApp(HEAD_LFS, sizeof HEAD_LFS));
  check("a coredump is not mistaken for an app", !otaSlotLooksLikeApp(HEAD_OTHER, sizeof HEAD_OTHER));

  // 0xE9 on its own is not enough: it is one byte, and plenty of flash content starts with it.
  uint8_t teaser[0x80] = {0};
  teaser[0] = 0xE9;
  check("0xE9 without an app descriptor is not an app", !otaSlotLooksLikeApp(teaser, sizeof teaser));
  // ...and a descriptor without the image magic is not one either (e.g. a half-erased slot).
  uint8_t headless[0x80];
  memcpy(headless, HEAD_APP, sizeof headless);
  headless[0] = 0xFF;
  check("an app descriptor without 0xE9 is not an app", !otaSlotLooksLikeApp(headless, sizeof headless));
  check("a short read is never an app", !otaSlotLooksLikeApp(HEAD_APP, 8));
}

static void labels() {
  printf("  naming the image that is being protected\n");
  char buf[40];
  otaSlotDescText(HEAD_APP, sizeof HEAD_APP, OTA_SLOT_DESC_PROJECT, buf, sizeof buf);
  check("project_name comes out", strcmp(buf, "arduino-lib-builder") == 0);
  otaSlotDescText(HEAD_APP, sizeof HEAD_APP, OTA_SLOT_DESC_VERSION, buf, sizeof buf);
  check("version comes out", strcmp(buf, "esp-idf: v4.4.7 38eeba213a") == 0);

  char tiny[8];
  otaSlotDescText(HEAD_APP, sizeof HEAD_APP, OTA_SLOT_DESC_PROJECT, tiny, sizeof tiny);
  check("a short buffer truncates instead of overflowing", strcmp(tiny, "arduino") == 0);
  otaSlotDescText(HEAD_APP, 0x60, OTA_SLOT_DESC_PROJECT, buf, sizeof buf);
  check("too few bytes read -> empty, not garbage", buf[0] == 0);

  // Raw flash bytes must never reach a JSON string or the page: only printable ASCII survives.
  uint8_t noisy[0x80];
  memcpy(noisy, HEAD_APP, sizeof noisy);
  uint32_t at = OTA_SLOT_DESC_OFF + OTA_SLOT_DESC_PROJECT;
  noisy[at + 0] = 'A';
  noisy[at + 1] = 0x01;
  noisy[at + 2] = '"';
  noisy[at + 3] = 0x80;
  noisy[at + 4] = 'Z';
  noisy[at + 5] = 0;
  otaSlotDescText(noisy, sizeof noisy, OTA_SLOT_DESC_PROJECT, buf, sizeof buf);
  check("unprintable bytes are dropped", strcmp(buf, "A\"Z") == 0);
}

// The reason this file exists: the boots the gateway can wake up into, and what has to happen in each.
// Only the first one may leave the slot alone -- the others all worked before the guard existed and have
// to keep working, or history silently drops to the 128 KB fallback for everybody.
static void decisions() {
  printf("  the decision in the boots that actually occur\n");
  OtaSlotState freshConversion = {false, true, false, false};
  check("just converted: the stock image is in the slot -> HOLD", !otaSlotClaimable(freshConversion));

  OtaSlotState everyday = {true, false, true, false};
  check("the store is already there -> claim", otaSlotClaimable(everyday));
  OtaSlotState everydayNoMarker = {true, false, false, false};
  check("...even with no marker yet (an install predating the guard)", otaSlotClaimable(everydayNoMarker));

  OtaSlotState afterSelfUpdate = {false, true, true, false};
  check("one update later: our own superseded image -> claim", otaSlotClaimable(afterSelfUpdate));

  OtaSlotState blankSlot = {false, false, false, false};
  check("a slot that was never flashed -> claim", otaSlotClaimable(blankSlot));

  OtaSlotState released = {false, true, false, true};
  check("the user released it on purpose -> claim", otaSlotClaimable(released));

  // The store check wins over everything else: if our filesystem is in there, any image that was there is
  // long gone anyway, and refusing to mount would only lose the data that IS there.
  OtaSlotState storeAndImageBytes = {true, true, false, false};
  check("store magic beats a stray app signature", otaSlotClaimable(storeAndImageBytes));
}

int main() {
  printf("ota_slot_guard.h -- spare-slot rules (issue #100)\n");
  contentProbes();
  labels();
  decisions();
  if (fail) { printf("=> FAILURES\n"); return 1; }
  printf("=> all invariants hold\n");
  return 0;
}
