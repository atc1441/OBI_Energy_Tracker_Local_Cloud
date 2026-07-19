// ota_hist.h — raw energy-sample storage in the unused space of the inactive OTA app partition.
// -----------------------------------------------------------------------------------------------
// The self-OTA app partitions (ota_0/ota_1) exist in pairs so the bootloader can always flash into
// whichever one ISN'T currently running. On obi_gateway_c3 each is ~1.9 MB but the firmware itself is
// only ~1.5 MB — several hundred KB just sits there unused in whichever partition isn't booted. This
// uses that space for the reader energy history's raw samples (the "/s<id>" files -- see the "energy
// history" section of gateway_web.cpp) instead of the 128 KB "userdata" partition alone. Daily summaries
// ("/d<id>") stay in "userdata" unconditionally, on every board: they're tiny (a few KB per reader even
// at 240 days) and matter for the long term, so they get the one storage location that's NEVER resized,
// reformatted, or at risk from any of what follows -- only the disposable, easily-regenerated raw samples
// live in this more eventful spot. See GitHub issue #36 for the design discussion this implements.
//
// TWO regions, mounted simultaneously, on whichever ota_x partition is CURRENTLY INACTIVE:
//
//   BULK (OTA_HIST_BASE):      [partition_start, partition_end - OTA_HIST_TAIL_RESERVE) -- basically the
//                               WHOLE unused partition, ~1.6 MB. Primary store: every read (the /history
//                               page, CSV export, ...) and every write goes here. Does NOT survive an OTA
//                               update -- Update.write() physically overwrites this address range with the
//                               new firmware, same as it always would (firmware occupies the front of
//                               whichever partition it's flashed into; there's no way around that and no
//                               attempt is made to protect this region). Reformats itself automatically
//                               the first time it's mounted after that (finds firmware bytes instead of a
//                               filesystem, format_if_mount_failed kicks in), then gets "seeded" with
//                               whatever survived in TAIL so continuity feels seamless.
//
//   TAIL (OTA_HIST_TAIL_BASE): the last OTA_HIST_TAIL_RESERVE bytes of the partition -- a small, ALWAYS
//                               fixed-address, fixed-size mirror of the newest data. Every write to BULK
//                               also writes here (see gateway_web.cpp's appendSample()), with its own much
//                               smaller per-file cap. This is the ONLY thing guaranteed to survive an OTA:
//                               Update.write() never erases/writes past the real firmware image length,
//                               and TAIL's fixed address is comfortably past any realistic firmware size,
//                               so it survives the flash into that partition untouched, automatically. What
//                               changes after the reboot is which partition is "active" vs. "inactive": the
//                               survived data is now sitting in the tail of the partition just booted INTO
//                               (now active), not the one used for day-to-day reads/writes (whatever is
//                               currently inactive). The first otaHist_mount() after that migrates it
//                               across -- reading ONLY from the active partition's tail (never erasing/
//                               writing it: doing so crashes real obi_gateway_c3 hardware, confirmed live --
//                               see ota_hist.cpp) into the new inactive partition's tail -- then seeds BULK
//                               with the same data so the primary store picks up right where TAIL left off.
//
// Net effect: "use as much of the partition as possible day to day" (BULK) + "an OTA update only loses
// whatever doesn't fit in the guaranteed-safe reserve" (TAIL, and BULK reseeded from it) -- not "lose
// everything below a fixed reserve on every single update" (the old, one-region design) or "silently
// corrupt data that happened to be in the way" (attempting to shrink one big region in place -- not
// pursued: needs staging as much data as the whole reserve in RAM at once, more than this chip has free).
//
// Only meaningful on obi_gateway_c3 (the one board with a known, verified dual-OTA layout this was
// designed against — see platformio.ini). Every function here is a safe no-op / returns false on other
// boards, which keep using the existing small "userdata" partition for both daily and raw data, exactly
// as before this feature existed.
#pragma once
#include <Arduino.h>

#ifdef OBI_BOARD_OBI_C3
#define OBI_OTA_HIST_SUPPORTED 1
#endif

#define OTA_HIST_BASE      "/otahist"       // bulk region -- reads/writes/CSV export/directory listing go here
#define OTA_HIST_TAIL_BASE "/otahisttail"   // small fixed mirror of the newest data -- survives OTA updates

// Fixed size of the tail mirror -- the only thing guaranteed to survive an OTA update. Must stay small
// enough to comfortably fit under ANY realistic firmware size headroom (see OTA_MIN_FW_BUDGET in
// ota_hist.cpp), and must never move: existing on-flash data there is only found again by mounting at
// this exact same fixed offset from the partition's end.
static const uint32_t OTA_HIST_TAIL_RESERVE = 256UL * 1024UL;

bool   otaHist_mount();            // mount both regions (format-on-first-use) on the current inactive partition
bool   otaHist_mounted();          // true if the BULK region is mounted (the one callers actually read/write)
bool   otaHist_format();           // wipe + remount both regions (factory reset)

// Called right before Update.begin() in BOTH OTA entry points (self-update upload, GitHub auto-update):
// pauses history writes and releases both VFS handles on the partition Update.begin() is about to
// overwrite. No relocation happens here -- TAIL's data already survives the flash in place (see the big
// comment above) and gets migrated forward, then reseeds BULK, on the next boot. Always safe; never
// blocks the flash either way.
bool   otaHist_prepareForFlash();
void   otaHist_abortFlash();       // OTA failed/rejected and device keeps running current firmware -- remount
bool   otaHist_writesPaused();     // historyService() must skip logging raw samples while true

void   otaHist_space(size_t &total, size_t &used);          // BULK region space; 0/0 if not mounted
void   otaHist_regionInfo(uint32_t &addr, uint32_t &size);   // BULK region physical address/size (0/0 if none)
