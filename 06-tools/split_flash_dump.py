#!/usr/bin/env python3
# split_flash_dump.py — carve a full ESP32 flash dump into per-partition .bin files.
#
# Feed it the `flash_full_*.bin` you downloaded from the custom firmware's /debug page
# ("Dump full flash"). It reads the ESP32 partition table at 0x8000, then writes every
# partition (bootloader, nvs, otadata, ota_0, ota_1, spiffs/userdata, …) to its own file —
# so the two app slots come out as separate images. One of ota_0/ota_1 is the running
# firmware, the other is the PREVIOUS version the last OTA left behind.
#
# Pure stdlib, no esptool needed:
#   python split_flash_dump.py flash_full_4096k.bin
#   python split_flash_dump.py flash_full_4096k.bin -o out/ --only ota_0,ota_1
#
# The dump must start at flash offset 0x0 (the /debug "Dump full flash" button does exactly
# that). A single-partition "Save .bin" file is already one partition — no split needed.

import argparse, os, struct, sys

PART_TABLE_OFFSET = 0x8000        # fixed ESP32 partition-table location
PART_TABLE_MAX    = 0xC00         # 3 KB -> up to 95 entries + md5
ENTRY_MAGIC       = 0x50AA        # 0xAA 0x50, little-endian u16
MD5_MAGIC         = 0xEBEB        # marks the checksum entry that ends the table

TYPE = {0: "app", 1: "data"}
APP_SUB  = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x20: "test"}
DATA_SUB = {0x00: "otadata", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
            0x04: "nvs_keys", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs"}


def parse_table(img):
    """Yield (label, type_str, sub_str, offset, size) for each partition entry."""
    parts, pos = [], PART_TABLE_OFFSET
    end = PART_TABLE_OFFSET + PART_TABLE_MAX
    while pos + 32 <= min(end, len(img)):
        magic = struct.unpack_from("<H", img, pos)[0]
        if magic == MD5_MAGIC:
            break                                   # md5 row -> end of table
        if magic != ENTRY_MAGIC:
            break                                   # unused/blank -> end of table
        ptype, psub = img[pos + 2], img[pos + 3]
        off, size = struct.unpack_from("<II", img, pos + 4)
        label = img[pos + 12:pos + 28].split(b"\x00", 1)[0].decode("latin1")
        tstr = TYPE.get(ptype, f"0x{ptype:02X}")
        sub = (APP_SUB if ptype == 0 else DATA_SUB).get(psub, f"0x{psub:02X}")
        parts.append((label or sub, tstr, sub, off, size))
        pos += 32
    return parts


def main():
    ap = argparse.ArgumentParser(description="Split an ESP32 full-flash dump into per-partition .bin files.")
    ap.add_argument("dump", help="flash_full_*.bin from the /debug 'Dump full flash' button")
    ap.add_argument("-o", "--outdir", default=".", help="output directory (default: current)")
    ap.add_argument("--only", help="comma-separated labels to extract (e.g. ota_0,ota_1)")
    ap.add_argument("--list", action="store_true", help="only print the partition table, write nothing")
    args = ap.parse_args()

    with open(args.dump, "rb") as f:
        img = f.read()

    parts = parse_table(img)
    if not parts:
        sys.exit(f"no partition table found at 0x{PART_TABLE_OFFSET:X} — is this a full dump starting at 0x0?")

    wanted = set(args.only.split(",")) if args.only else None
    os.makedirs(args.outdir, exist_ok=True)

    print(f"{'label':<12} {'type':<5} {'subtype':<8} {'offset':>10} {'size':>10}  file")
    for label, tstr, sub, off, size in parts:
        note = ""
        if off + size > len(img):
            note = "  ⚠ extends past dump — truncated"
            size = max(0, len(img) - off)
        if args.list or (wanted and label not in wanted):
            print(f"{label:<12} {tstr:<5} {sub:<8} 0x{off:08X} 0x{size:08X}{note}")
            continue
        out = os.path.join(args.outdir, f"{label}.bin")
        with open(out, "wb") as w:
            w.write(img[off:off + size])
        print(f"{label:<12} {tstr:<5} {sub:<8} 0x{off:08X} 0x{size:08X}  -> {out}{note}")


if __name__ == "__main__":
    main()
