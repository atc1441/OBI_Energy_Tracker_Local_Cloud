// flash_dbg.cpp — raw flash read/write/erase + a UART command console (see flash_dbg.h).
#include "flash_dbg.h"
#include <Arduino.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <MD5Builder.h>
#include <soc/rtc_cntl_reg.h>
#include <nvs_flash.h>
#include <ctype.h>

#define FDBG_SECTOR 4096u

#ifdef OBI_FLASH_FORCE_DIO
// The OBI board's flash is wired 2-line (DIO), but the Arduino C3 libs are built QIO. If the first NVS
// read happens in QIO mode (Arduino's nvs_flash_init, during initArduino) it sees garbage, decides the
// partition is corrupt and REFORMATS it — so settings/bindings written in DIO never survive a reboot.
// Fix at the earliest possible point: a global constructor runs before app_main()/initArduino(), so the
// esp_flash driver is already DIO before anything reads NVS. esp_flash_default_chip is set up in
// esp_startup (before C++ ctors) and the FreeRTOS scheduler is up, so this is safe here.
struct ObiDioForcer {
  ObiDioForcer() {
    if (esp_flash_default_chip) {
      esp_flash_default_chip->read_mode = SPI_FLASH_DIO;
      esp_flash_init(esp_flash_default_chip);
    }
  }
};
static ObiDioForcer g_obiDioForcer;
#endif

void flash_dbg_force_dio() {
#ifdef OBI_FLASH_FORCE_DIO
  if (!esp_flash_default_chip) return;
  esp_flash_default_chip->read_mode = SPI_FLASH_DIO;      // 2-data-line reads (matches the OBI board wiring)
  esp_err_t e = esp_flash_init(esp_flash_default_chip);   // reconfigure the driver with the new read mode
  Serial.printf("[dbg] forced flash read-mode DIO (esp_flash_init=%d)\n", e);
  // The early boot NVS init read the partition in QIO (garbage); rebuild it now that reads are correct.
  nvs_flash_deinit();
  esp_err_t n = nvs_flash_init();
  if (n == ESP_ERR_NVS_NO_FREE_PAGES || n == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); n = nvs_flash_init(); }
  Serial.printf("[dbg] NVS re-init after DIO fix=%d\n", n);
#endif
}

uint32_t flash_dbg_size() {
  uint32_t sz = 0;
  if (esp_flash_get_size(esp_flash_default_chip, &sz) != ESP_OK) return 0;
  return sz;
}

bool flash_dbg_read(uint32_t addr, uint8_t *buf, size_t len) {
  return esp_flash_read(esp_flash_default_chip, buf, addr, len) == ESP_OK;
}

bool flash_dbg_write(uint32_t addr, const uint8_t *buf, size_t len) {
  return esp_flash_write(esp_flash_default_chip, buf, addr, len) == ESP_OK;
}

bool flash_dbg_erase(uint32_t addr, size_t len) {
  uint32_t start = addr & ~(FDBG_SECTOR - 1);
  uint32_t end   = (addr + len + FDBG_SECTOR - 1) & ~(FDBG_SECTOR - 1);
  if (end <= start) end = start + FDBG_SECTOR;
  return esp_flash_erase_region(esp_flash_default_chip, start, end - start) == ESP_OK;
}

// Change arbitrary bytes: for each affected 4 KB sector, read it, apply the edit in RAM, erase the
// sector, write it back — so untouched bytes in the sector are preserved (NOR flash can't set bits).
bool flash_dbg_patch(uint32_t addr, const uint8_t *buf, size_t len) {
  uint8_t *sec = (uint8_t *)malloc(FDBG_SECTOR);
  if (!sec) return false;
  bool ok = true;
  uint32_t pos = addr, remaining = len;
  const uint8_t *src = buf;
  while (remaining && ok) {
    uint32_t base = pos & ~(FDBG_SECTOR - 1);
    uint32_t off  = pos - base;
    uint32_t n    = FDBG_SECTOR - off;
    if (n > remaining) n = remaining;
    ok = flash_dbg_read(base, sec, FDBG_SECTOR)
      && esp_flash_erase_region(esp_flash_default_chip, base, FDBG_SECTOR) == ESP_OK;
    if (ok) { memcpy(sec + off, src, n); ok = flash_dbg_write(base, sec, FDBG_SECTOR); }
    pos += n; src += n; remaining -= n;
  }
  free(sec);
  return ok;
}

// ------------------------------------------------------------------ eFuse / security
struct EfuseKV { const char *name; int val; };

// Fill a small table of the security-relevant eFuses for whatever chip we're built for.
static int fillEfuses(EfuseKV *o, int max) {
  int n = 0;
  auto add = [&](const char *nm, int v) { if (n < max) { o[n].name = nm; o[n].val = v; } n++; };
#if defined(CONFIG_IDF_TARGET_ESP32)
  { uint8_t c = 0; esp_efuse_read_field_blob(ESP_EFUSE_FLASH_CRYPT_CNT, &c, 7); add("FLASH_CRYPT_CNT", c); }
  add("UART_DOWNLOAD_DIS", esp_efuse_read_field_bit(ESP_EFUSE_UART_DOWNLOAD_DIS));
  add("ABS_DONE_0(SBv1)",  esp_efuse_read_field_bit(ESP_EFUSE_ABS_DONE_0));
  add("ABS_DONE_1(SBv2)",  esp_efuse_read_field_bit(ESP_EFUSE_ABS_DONE_1));
#else
  add("DIS_DOWNLOAD_MODE", esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE));
  add("DIS_DIRECT_BOOT",   esp_efuse_read_field_bit(ESP_EFUSE_DIS_DIRECT_BOOT));
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // On the S3 the pad-JTAG fuse is split into HARD/SOFT (DIS_PAD_JTAG was
  // removed from the efuse table in newer IDF releases).
  add("HARD_DIS_JTAG",     esp_efuse_read_field_bit(ESP_EFUSE_HARD_DIS_JTAG));
  add("SOFT_DIS_JTAG",     esp_efuse_read_field_bit(ESP_EFUSE_SOFT_DIS_JTAG));
#else
  add("DIS_PAD_JTAG",      esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG));
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
  add("DIS_USB_JTAG",      esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG));
#endif
  add("SECURE_BOOT_EN",    esp_efuse_read_field_bit(ESP_EFUSE_SECURE_BOOT_EN));
  add("SECURE_DL_EN",      esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD));
  { uint8_t c = 0; esp_efuse_read_field_blob(ESP_EFUSE_SPI_BOOT_CRYPT_CNT, &c, 3); add("SPI_BOOT_CRYPT_CNT", c); }
#endif
  return n;
}

bool flash_dbg_download_locked() {
#if defined(CONFIG_IDF_TARGET_ESP32)
  return esp_efuse_read_field_bit(ESP_EFUSE_UART_DOWNLOAD_DIS);
#else
  return esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);
#endif
}

String flash_dbg_efuse_json() {
  EfuseKV kv[16];
  int n = fillEfuses(kv, 16);
  String j = "{\"chip\":\"" + String(ESP.getChipModel()) + "\",\"rev\":" + String(ESP.getChipRevision());
  uint64_t mac = ESP.getEfuseMac();
  char m[13]; snprintf(m, sizeof m, "%012llX", mac);
  j += ",\"mac\":\"" + String(m) + "\",\"dl_locked\":" + String(flash_dbg_download_locked() ? "true" : "false") + ",\"fuses\":{";
  for (int i = 0; i < n; i++) { if (i) j += ","; j += "\"" + String(kv[i].name) + "\":" + String(kv[i].val); }
  j += "}}";
  return j;
}

void flash_dbg_efuse_print(Print &o) {
  EfuseKV kv[16];
  int n = fillEfuses(kv, 16);
  o.printf("chip: %s rev%d  mac:%012llX\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getEfuseMac());
  o.printf("ROM download mode: %s\n", flash_dbg_download_locked() ? "DISABLED (locked)" : "available");
  for (int i = 0; i < n; i++) o.printf("  %-20s = %d\n", kv[i].name, kv[i].val);
}

// Jump into the chip's ROM UART download mode (esptool can then connect) — only if not fused off.
bool flash_dbg_enter_download() {
  if (flash_dbg_download_locked()) return false;
#if defined(RTC_CNTL_OPTION1_REG) && defined(RTC_CNTL_FORCE_DOWNLOAD_BOOT)
  REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);  // survives the reset; ROM enters DL mode
  delay(50);
  esp_restart();
  return true;   // not reached
#else
  return false;  // software-forced download boot not available on this chip (e.g. classic ESP32)
#endif
}

bool flash_dbg_md5(uint32_t addr, uint32_t len, char out[33]) {
  MD5Builder md5; md5.begin();
  uint8_t buf[512];
  uint32_t pos = 0;
  while (pos < len) {
    uint32_t n = (len - pos < sizeof buf) ? (len - pos) : sizeof buf;
    if (!flash_dbg_read(addr + pos, buf, n)) return false;
    md5.add(buf, n);
    pos += n;
  }
  md5.calculate();
  md5.getChars(out);
  return true;
}

// ------------------------------------------------------------------ UART console
static char   s_line[576];
static size_t s_len = 0;

static uint32_t parseNum(const char *s) { return (uint32_t)strtoul(s, nullptr, 0); }  // 0x.. hex or decimal

// parse a run of hex bytes ("de ad be ef" or "deadbeef") into out[]; returns count
static int parseHex(const char *s, uint8_t *out, int maxOut) {
  int n = 0;
  while (*s && n < maxOut) {
    while (*s == ' ' || *s == ',' || *s == ':') s++;
    if (!isxdigit((unsigned char)*s)) break;
    char hi = *s++;
    char lo = isxdigit((unsigned char)*s) ? *s++ : '0';
    char b[3] = { hi, lo, 0 };
    out[n++] = (uint8_t)strtol(b, nullptr, 16);
  }
  return n;
}

static void dumpFlash(uint32_t addr, uint32_t len) {
  uint8_t row[16];
  for (uint32_t o = 0; o < len; o += 16) {
    uint32_t n = (len - o < 16) ? (len - o) : 16;
    if (!flash_dbg_read(addr + o, row, n)) { Serial.println("read error"); return; }
    Serial.printf("%08X  ", addr + o);
    for (uint32_t i = 0; i < 16; i++) {
      if (i < n) Serial.printf("%02X ", row[i]); else Serial.print("   ");
      if (i == 7) Serial.print(' ');
    }
    Serial.print(" |");
    for (uint32_t i = 0; i < n; i++) { char c = row[i]; Serial.print((c >= 32 && c < 127) ? c : '.'); }
    Serial.println("|");
  }
}

static void printPartitions() {
  Serial.printf("flash size: %u bytes (%u KB)\n", flash_dbg_size(), flash_dbg_size() / 1024);
  Serial.println("partitions:");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    Serial.printf("  %-10s type=%u sub=0x%02X  off=0x%06X  size=0x%06X (%uK)\n",
                  p->label, p->type, p->subtype, p->address, p->size, p->size / 1024);
  }
  esp_partition_iterator_release(it);
}

static void execCmd(char *line) {
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    Serial.println("flash console:");
    Serial.println("  rd <addr> [len]        hexdump flash (len default 256)");
    Serial.println("  wr <addr> <hex...>     write bytes (read-modify-erase-write, safe for any offset)");
    Serial.println("  er <addr> [len]        erase (rounded to 4 KB sectors; len default 4096)");
    Serial.println("  md5 <addr> <len>       MD5 of a flash range (verify an image)");
    Serial.println("  info | size            partition table / chip size");
    Serial.println("  efuse                  chip + security eFuses (download-mode lock, JTAG, secure boot)");
    Serial.println("  download               jump to ROM UART download mode (only if not fused off)");
    Serial.println("  reboot                 restart the device");
    Serial.println("  numbers are 0x-hex or decimal.  WARNING: writing the wrong offset can brick it.");
    return;
  }
  if (!strcmp(cmd, "info")) { printPartitions(); return; }
  if (!strcmp(cmd, "size")) { Serial.printf("flash size: %u bytes\n", flash_dbg_size()); return; }
  if (!strcmp(cmd, "efuse")) { flash_dbg_efuse_print(Serial); return; }
  if (!strcmp(cmd, "download")) {
    if (flash_dbg_download_locked()) { Serial.println("ROM download mode is DISABLED by eFuse — cannot enter it"); return; }
    Serial.println("entering ROM UART download mode — connect esptool now...");
    if (!flash_dbg_enter_download()) Serial.println("(software-forced download boot not supported on this chip)");
    return;
  }
  if (!strcmp(cmd, "md5")) {
    char *a = strtok(nullptr, " "), *l = strtok(nullptr, " ");
    if (!a || !l) { Serial.println("usage: md5 <addr> <len>"); return; }
    char hex[33];
    bool ok = flash_dbg_md5(parseNum(a), parseNum(l), hex);
    if (ok) Serial.printf("md5(0x%08lX, %lu) = %s\n", strtoul(a, nullptr, 0), strtoul(l, nullptr, 0), hex);
    else Serial.println("md5 read error");
    return;
  }
  if (!strcmp(cmd, "reboot") || !strcmp(cmd, "restart")) { Serial.println("rebooting..."); delay(100); ESP.restart(); return; }

  if (!strcmp(cmd, "rd")) {
    char *a = strtok(nullptr, " "), *l = strtok(nullptr, " ");
    if (!a) { Serial.println("usage: rd <addr> [len]"); return; }
    uint32_t addr = parseNum(a), len = l ? parseNum(l) : 256;
    if (len > 8192) len = 8192;
    dumpFlash(addr, len);
    return;
  }
  if (!strcmp(cmd, "er")) {
    char *a = strtok(nullptr, " "), *l = strtok(nullptr, " ");
    if (!a) { Serial.println("usage: er <addr> [len]"); return; }
    uint32_t addr = parseNum(a), len = l ? parseNum(l) : FDBG_SECTOR;
    bool ok = flash_dbg_erase(addr, len);
    Serial.printf("erase @0x%08X len %u -> %s\n", addr, len, ok ? "ok" : "FAIL");
    return;
  }
  if (!strcmp(cmd, "wr") || !strcmp(cmd, "patch")) {
    char *a = strtok(nullptr, " ");
    char *rest = strtok(nullptr, "");           // everything after the address = hex bytes
    if (!a || !rest) { Serial.println("usage: wr <addr> <hex bytes>"); return; }
    uint32_t addr = parseNum(a);
    static uint8_t buf[512];
    int n = parseHex(rest, buf, sizeof buf);
    if (n <= 0) { Serial.println("no hex bytes given"); return; }
    bool ok = flash_dbg_patch(addr, buf, n);
    Serial.printf("write %d byte(s) @0x%08X -> %s\n", n, addr, ok ? "ok" : "FAIL");
    return;
  }
  Serial.printf("unknown cmd '%s' — type 'help'\n", cmd);
}

void flash_dbg_uart_begin() {
  Serial.printf("[dbg] UART flash console ready (type 'help'). flash=%u bytes\n", flash_dbg_size());
}

void flash_dbg_uart_poll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { s_line[s_len] = 0; if (s_len) execCmd(s_line); s_len = 0; }
    else if (s_len < sizeof(s_line) - 1) s_line[s_len++] = c;
  }
}
