#include "ota_hist.h"

#ifdef OBI_OTA_HIST_SUPPORTED

#include <esp_partition.h>
#include <esp_ota_ops.h>
extern "C" {
#include <esp_littlefs.h>
}
#include <Preferences.h>   // tiny "which partition did we last mount" marker -- see needsMigrate()
#include <dirent.h>

// Sanity floor: the partition must be big enough for the tail reserve plus a realistic firmware image,
// or this board's partition table doesn't match what this feature was designed against (see platformio.ini).
static const uint32_t OTA_MIN_FW_BUDGET = 900UL * 1024UL;

static bool            g_bulkMounted = false;
static bool            g_tailMounted = false;
static bool            g_paused      = false;
static esp_partition_t g_bulkPart;   // sub-partition structs currently registered with littlefs -- kept
static esp_partition_t g_tailPart;   // alive for as long as each mount exists (register may keep
                                      // referencing the pointer, not just copy out of it at call time)

static esp_partition_t bulkRegionOf(const esp_partition_t *ota) {
  esp_partition_t p = *ota;
  p.size = ota->size - OTA_HIST_TAIL_RESERVE;
  p.type = ESP_PARTITION_TYPE_DATA;
  strlcpy(p.label, "otahistbulk", sizeof p.label);
  return p;
}
static esp_partition_t tailRegionOf(const esp_partition_t *ota) {
  esp_partition_t p = *ota;
  p.address = ota->address + ota->size - OTA_HIST_TAIL_RESERVE;
  p.size    = OTA_HIST_TAIL_RESERVE;
  p.type    = ESP_PARTITION_TYPE_DATA;
  strlcpy(p.label, "otahisttail", sizeof p.label);
  return p;
}
static bool sizeOk(const esp_partition_t *ota) {
  return ota && ota->size >= OTA_HIST_TAIL_RESERVE + OTA_MIN_FW_BUDGET;
}

static void unmountBulk() { if (g_bulkMounted) { esp_vfs_littlefs_unregister_partition(&g_bulkPart); g_bulkMounted = false; } }
static void unmountTail() { if (g_tailMounted) { esp_vfs_littlefs_unregister_partition(&g_tailPart); g_tailMounted = false; } }

// Stream-copy every file from one already-mounted directory to another, trimming each file to the newest
// bytes that fit within a shrinking total budget (same "keep newest, snap to a line boundary" convention
// as the ongoing per-file compaction in appendSample(), gateway_web.cpp). Small, bounded buffers only --
// never holds a whole region's worth of data in RAM.
static void copyDir(const char *srcBase, const char *dstBase, uint32_t budget) {
  DIR *dir = opendir(srcBase);
  if (!dir) return;
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr && budget > 0) {
    String name = de->d_name;
    String sp = String(srcBase) + "/" + name;
    FILE *sf = fopen(sp.c_str(), "rb");
    if (!sf) continue;
    fseek(sf, 0, SEEK_END); long sz = ftell(sf); fseek(sf, 0, SEEK_SET);
    String all; all.reserve(sz > 0 ? sz : 0);
    { uint8_t buf[512]; size_t n; while ((n = fread(buf, 1, sizeof buf, sf)) > 0) all.concat((const char *)buf, n); }
    fclose(sf);
    String out = all;
    if ((uint32_t)all.length() > budget) {
      int cut = (int)all.length() - (int)budget;
      int nl = all.indexOf('\n', cut > 0 ? cut : 0);
      out = (nl >= 0) ? all.substring(nl + 1) : String();
    }
    if (out.length()) {
      String dp = String(dstBase) + "/" + name;
      FILE *df = fopen(dp.c_str(), "wb");
      if (df) { fwrite(out.c_str(), 1, out.length(), df); fclose(df); }
      budget -= (out.length() > budget) ? budget : (uint32_t)out.length();
    }
  }
  closedir(dir);
}

// Copy TAIL's newest data (the only thing guaranteed to have survived the last OTA) from the ACTIVE
// partition into the just-formatted, already-mounted TAIL on the (currently inactive) partition -- called
// only when otaHist_mount() has determined (via the NVS marker) that active/inactive flipped since last
// boot. READS ONLY from the active partition (a plain read-only mount); only ever erases/writes the
// inactive one. See ota_hist.h for why that split matters on this hardware.
static void migrateTailFromActive() {
  const esp_partition_t *active = esp_ota_get_running_partition();
  if (!sizeOk(active)) return;
  esp_partition_t srcTail = tailRegionOf(active);

  esp_vfs_littlefs_conf_t sconf = {};
  sconf.base_path = "/otahist_src";
  sconf.partition_label = nullptr;
  sconf.partition = &srcTail;
  sconf.format_if_mount_failed = false;   // read-only probe -- never write/format the active partition
  sconf.read_only = true;
  sconf.dont_mount = false;
  if (esp_vfs_littlefs_register(&sconf) != ESP_OK) return;   // nothing valid there (first-ever activation)

  copyDir("/otahist_src", OTA_HIST_TAIL_BASE, OTA_HIST_TAIL_RESERVE);
  esp_vfs_littlefs_unregister_partition(&srcTail);
  Serial.println("[otahist] migrated the newest raw samples forward across the last OTA update");
}

bool otaHist_mount() {
  unmountBulk(); unmountTail();
  const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
  if (!sizeOk(inactive)) return false;

  Preferences pr; pr.begin("otahist", true);
  uint32_t lastAddr = pr.getUInt("addr", 0);
  pr.end();
  bool needMigrate = (lastAddr != 0 && lastAddr != inactive->address);

  // TAIL first: fixed address/size, always -- start clean on a migration (an earlier stint of this SAME
  // partition as "inactive" may have left stale data there from 1+ OTA cycles ago).
  g_tailPart = tailRegionOf(inactive);
  if (needMigrate) esp_littlefs_format_partition(&g_tailPart);
  esp_vfs_littlefs_conf_t tconf = {};
  tconf.base_path = OTA_HIST_TAIL_BASE;
  tconf.partition_label = nullptr;
  tconf.partition = &g_tailPart;
  tconf.format_if_mount_failed = true;
  tconf.dont_mount = false;
  tconf.grow_on_mount = true;
  g_tailMounted = (esp_vfs_littlefs_register(&tconf) == ESP_OK);
  if (needMigrate && g_tailMounted) migrateTailFromActive();

  // BULK: nearly the whole partition. If an OTA just happened, its old content is real firmware bytes now
  // (Update.write() physically overwrote this exact range -- there's no way around that, see ota_hist.h),
  // so the mount naturally fails and format_if_mount_failed reformats it fresh; then seed it from TAIL
  // (small, just migrated) so the primary store isn't left empty right after an update.
  g_bulkPart = bulkRegionOf(inactive);
  esp_vfs_littlefs_conf_t bconf = {};
  bconf.base_path = OTA_HIST_BASE;
  bconf.partition_label = nullptr;
  bconf.partition = &g_bulkPart;
  bconf.format_if_mount_failed = true;
  bconf.dont_mount = false;
  bconf.grow_on_mount = true;
  g_bulkMounted = (esp_vfs_littlefs_register(&bconf) == ESP_OK);
  if (!g_bulkMounted) { Serial.println("[otahist] bulk mount failed"); unmountTail(); return false; }

  if (needMigrate && g_tailMounted) copyDir(OTA_HIST_TAIL_BASE, OTA_HIST_BASE, OTA_HIST_TAIL_RESERVE);

  { Preferences pp; pp.begin("otahist", false); pp.putUInt("addr", inactive->address); pp.end(); }
  return true;
}

bool otaHist_mounted() { return g_bulkMounted; }

bool otaHist_format() {
  if (g_bulkMounted) { esp_partition_t b = g_bulkPart; unmountBulk(); esp_littlefs_format_partition(&b); }
  if (g_tailMounted) { esp_partition_t t = g_tailPart; unmountTail(); esp_littlefs_format_partition(&t); }
  return otaHist_mount();
}
bool otaHist_writesPaused() { return g_paused; }

// Snapshot BULK's current content into TAIL right before an OTA -- "check the main raw store once and
// stash as much of it as fits," not a fixed slot per reader. Readers using less than an equal share of
// OTA_HIST_TAIL_RESERVE keep everything; the slack they don't need is redistributed evenly across whatever
// readers need more than their equal share -- so e.g. a single-reader setup gets the WHOLE reserve for
// that one reader, not an arbitrary small per-reader cap. Both BULK (read) and the freshly reformatted
// TAIL (write) are on the SAME currently-inactive partition -- never the active one (see the long comment
// below for why that split matters on this hardware).
static const int OTA_HIST_MAX_READERS = 64;   // sizing match for gateway_web.cpp's own HIST_SLOTS/MAX_READERS
static void snapshotBulkIntoTail() {
  static String names[OTA_HIST_MAX_READERS];
  static long   sizes[OTA_HIST_MAX_READERS];
  int n = 0; long total = 0;
  DIR *dir = opendir(OTA_HIST_BASE);
  if (dir) {
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr && n < OTA_HIST_MAX_READERS) {
      String name = de->d_name;
      if (name.length() != 7 || name[0] != 's') continue;   // "/s<6hex>" raw-sample files only
      String sp = String(OTA_HIST_BASE) + "/" + name;
      FILE *f = fopen(sp.c_str(), "rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      names[n] = name; sizes[n] = sz; total += sz; n++;
    }
    closedir(dir);
  }
  if (n == 0) return;

  uint32_t equalShare = OTA_HIST_TAIL_RESERVE / (uint32_t)n;
  long slack = 0; int needMore = 0;
  for (int i = 0; i < n; i++) {
    if (sizes[i] <= (long)equalShare) slack += (long)equalShare - sizes[i];
    else needMore++;
  }
  uint32_t boostedShare = needMore ? equalShare + (uint32_t)(slack / needMore) : equalShare;

  // Reformat TAIL fresh so an old, differently-sized snapshot doesn't linger, then remount it.
  unmountTail();
  esp_littlefs_format_partition(&g_tailPart);
  esp_vfs_littlefs_conf_t tconf = {};
  tconf.base_path = OTA_HIST_TAIL_BASE;
  tconf.partition_label = nullptr;
  tconf.partition = &g_tailPart;
  tconf.format_if_mount_failed = true;
  tconf.dont_mount = false;
  tconf.grow_on_mount = true;
  g_tailMounted = (esp_vfs_littlefs_register(&tconf) == ESP_OK);
  if (!g_tailMounted) return;

  for (int i = 0; i < n; i++) {
    uint32_t cap = sizes[i] <= (long)equalShare ? (uint32_t)sizes[i] : boostedShare;
    String sp = String(OTA_HIST_BASE) + "/" + names[i];
    FILE *sf = fopen(sp.c_str(), "rb");
    if (!sf) continue;
    String all; all.reserve(sizes[i] > 0 ? sizes[i] : 0);
    { uint8_t buf[512]; size_t rn; while ((rn = fread(buf, 1, sizeof buf, sf)) > 0) all.concat((const char *)buf, rn); }
    fclose(sf);
    String out = all;
    if ((uint32_t)all.length() > cap) {   // keep newest, snap to a line boundary (same convention as
      int cut = (int)all.length() - (int)cap;   // the ongoing per-file compaction in appendSample())
      int nl = all.indexOf('\n', cut > 0 ? cut : 0);
      out = (nl >= 0) ? all.substring(nl + 1) : String();
    }
    if (out.length()) {
      String dp = String(OTA_HIST_TAIL_BASE) + "/" + names[i];
      FILE *df = fopen(dp.c_str(), "wb");
      if (df) { fwrite(out.c_str(), 1, out.length(), df); fclose(df); }
    }
  }
  Serial.printf("[otahist] snapshotted %d reader(s) into the %lu KB tail reserve before flashing\n",
                n, (unsigned long)(OTA_HIST_TAIL_RESERVE / 1024));
}

// This USED to try to relocate history data into the active partition's tail here, before Update.begin()
// overwrites the inactive one -- turned out to be UNSAFE on real obi_gateway_c3 hardware: writing/erasing
// into the tail of the partition the CPU is CURRENTLY EXECUTING FROM crashes the device, confirmed two
// ways on the live unit -- a chunked (4 KB + yield) erase/copy into it crashed too, and so did a plain
// 64 KB erase into that same region via the pre-existing, unrelated /api/flash/erase debug endpoint
// (esp_flash_erase_region(), no LittleFS or ota_hist code involved at all). Not a bug in the chunking --
// general ESP-IDF docs say self-partition writes past the loaded image are fine, but this board's custom
// DIO-forced flash wiring (see flash_dbg.cpp) evidently doesn't tolerate it regardless.
//
// TAIL's fixed reserve is comfortably past any realistic firmware size, so Update.write() (verified
// against Updater.cpp: never erases/writes past the real image length) never reaches it -- whatever is
// sitting there survives the OTA untouched, automatically. BULK, by contrast, is NOT expected to survive
// -- it deliberately spans right up to where firmware will be written, to maximize day-to-day capacity
// (see ota_hist.h). So right before releasing both, snapshotBulkIntoTail() takes one fresh look at BULK's
// CURRENT content and fits as much of it as possible into TAIL (see that function's own comment for the
// fair-share split across readers) -- rather than TAIL being kept continuously in sync with a small fixed
// per-reader cap on every single sample, which wastes the reserve when there are only one or two readers
// and writes flash twice as often for no benefit between updates. migrateTailFromActive() (via
// otaHist_mount()) picks this snapshot back up one boot later, reading it from what is now the active
// partition (never erasing/writing there -- see above), then reseeds BULK from it.
bool otaHist_prepareForFlash() {
  g_paused = true;
  if (g_bulkMounted) snapshotBulkIntoTail();
  unmountBulk(); unmountTail();   // release both VFS handles -- Update.begin() is about to overwrite this partition
  return true;
}

void otaHist_abortFlash() {
  g_paused = false;
  otaHist_mount();   // OTA never completed -- re-resolves to the same (unchanged) inactive partition and
                      // remounts both; nothing here ever touched their contents, so this is the original data.
}

void otaHist_space(size_t &total, size_t &used) {
  total = used = 0;
  if (!g_bulkMounted) return;
  esp_littlefs_partition_info(&g_bulkPart, &total, &used);
}

void otaHist_regionInfo(uint32_t &addr, uint32_t &size) {
  addr = size = 0;
  if (!g_bulkMounted) return;
  addr = g_bulkPart.address;
  size = g_bulkPart.size;
}

#else  // !OBI_OTA_HIST_SUPPORTED -- other boards keep using the existing small "userdata" store untouched

bool otaHist_mount() { return false; }
bool otaHist_mounted() { return false; }
bool otaHist_format() { return false; }
bool otaHist_prepareForFlash() { return false; }
void otaHist_abortFlash() {}
bool otaHist_writesPaused() { return false; }
void otaHist_space(size_t &total, size_t &used) { total = used = 0; }
void otaHist_regionInfo(uint32_t &addr, uint32_t &size) { addr = size = 0; }

#endif
