import argparse
import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_stock_v57.bin"
DST = HERE / "reader_modded_89mock_v90_EMHeHZP.bin"
DST_SF9 = HERE / "reader_modded_89mock_v90_EMHeHZP_sf9.bin"
HOOKS_BIN = HERE / "build" / "hooks_EMHeHZP.bin"
HOOKS_SYM = HERE / "build" / "hooks_EMHeHZP.sym"
BASE = 0x4000

# --sf9: LoRa spreading factor SF7 -> SF9 (2026-07-21) -- see the matching comment in splice.py for the full
# writeup. Must be paired with the gateway's own SF setting (Settings page): flash this to the reader FIRST
# while the gateway is still on SF7 (the OTA transfer itself needs a working same-SF link), THEN switch the
# gateway to SF9.
SF9_PATCHES = [(0xB6D8, 0x07, 0x09), (0xB700, 0x07, 0x09), (0x66DA, 0x2D, 0xF5), (0x6C78, 0x07, 0x09)]

# EMH eHZ-P-specific variant (issue #68). Build with `./build.sh EMHeHZP`
# first (adds -DFIX_INT8_SIGN for entry.S; no C hook needed, see entry.S's
# entry_sxtb8_fix for the full writeup). Like the DWSB20.2TH variant below
# it, this fix ships as its own dedicated release/binary rather than folded
# into the shared build, since it's so far only been reported/confirmed on
# this one meter model.
HOOKS = [
    (0xC0EA, bytes([0x00, 0x20, 0xF8, 0xBD]), "entry_int24"),
    # SML TL=0x53 (Integer16) missing sign-extension fix (2026-07-11) -- see entry.S for the full writeup.
    # A general firmware defect, not specific to any one meter model, so it's in every variant's HOOKS list.
    (0xC110, bytes([0xC1, 0x17, 0x61, 0x60]), "entry_sxth16_fix"),
    # SML TL=0x52 (Integer8) missing sign-extension fix (2026-08-13, issue #68) -- see entry.S's
    # entry_sxtb8_fix. EMHeHZP-specific for now (see entry.S for why), unlike the Int16 fix above.
    (0xC0F2, bytes([0x21, 0x60, 0x60, 0x60]), "entry_sxtb8_fix"),
]


def bl_encode(src_addr, dst_addr):
    pc = src_addr + 4
    offset = dst_addr - pc
    assert offset % 2 == 0
    imm25 = offset // 2
    S = 1 if imm25 < 0 else 0
    if imm25 < 0:
        imm25 &= (1 << 24) - 1
    I1 = (imm25 >> 23) & 1
    I2 = (imm25 >> 22) & 1
    imm10 = (imm25 >> 11) & 0x3FF
    imm11 = imm25 & 0x7FF
    J1 = 1 ^ I1 ^ S
    J2 = 1 ^ I2 ^ S
    hw1 = 0xF000 | (S << 10) | imm10
    hw2 = 0xD000 | (J1 << 13) | (1 << 12) | (J2 << 11) | imm11
    return struct.pack("<HH", hw1, hw2)


def load_symbols(path):
    syms = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)", line.strip())
            if m:
                syms[m.group(2)] = int(m.group(1), 16) & ~1
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf9", action="store_true",
                     help="also patch the LoRa spreading factor SF7->SF9 (must match the gateway's own SF setting)")
    args = ap.parse_args()
    dst = DST_SF9 if args.sf9 else DST

    with open(SRC, "rb") as f:
        data = bytearray(f.read())
    orig_len = len(data)
    tramp_addr = BASE + orig_len

    with open(HOOKS_BIN, "rb") as f:
        hooks_bin = f.read()

    syms = load_symbols(HOOKS_SYM)
    print("symbols:", {k: hex(v) for k, v in syms.items()})

    assert tramp_addr == 0xEE08, f"base firmware length changed; update link.ld ORIGIN (expected 0xEE08, got {hex(tramp_addr)})"

    for addr, expect, sym in HOOKS:
        off = addr - BASE
        got = bytes(data[off:off + len(expect)])
        assert got == expect, f"@{hex(addr)}: expected {expect.hex()}, found {got.hex()} -- already patched or wrong base file?"
        target = syms[sym]
        patch = bl_encode(addr, target)
        print(f"patch @{hex(addr)}: BL -> {sym}@{hex(target)}  bytes={patch.hex(' ')}")
        data[off:off + len(patch)] = patch

    if args.sf9:
        for addr, old, new in SF9_PATCHES:
            off = addr - BASE
            assert data[off] == old, f"@{hex(addr)}: expected {old:#04x}, found {data[off]:#04x} -- already patched or wrong base file?"
            data[off] = new
            print(f"patch @{hex(addr)}: SF7->SF9 ({old:#04x} -> {new:#04x})")

    data += hooks_bin

    # softver 57 -> 89, release named/tagged "v90" -- the SAME mock-version pair as splice.py and
    # splice_DWSB20_2TH.py (see the comment above the equivalent patch in splice.py for the full
    # "why 89/v90" writeup). Deliberately does NOT get its own version number: the pair only needs to
    # differ from itself (89 != 90), not from the OTHER variants, and keeping it identical across every
    # variant means any release file -- plain, DWSB20.2TH, or this one -- can always be
    # (re)flashed onto a reader regardless of which variant (or version) it's currently running,
    # without ever hitting the "already this version, skipping" OTA no-op. Per-variant fixes are told
    # apart by which named .bin you flash, not by a version number -- so this number does not bump when
    # a new variant is added, only if the SHARED hooks (entry_int24, entry_sxth16_fix) themselves change.
    for off in (0x8b36, 0x8b80):
        assert data[off] == 0x39 and data[off + 1] == 0x20, f"unexpected bytes at {hex(off)}: {data[off:off+2].hex()}"
        data[off] = 0x59  # 0x59 = 89 decimal (intentionally != the v90 in DST's filename)

    with open(dst, "wb") as f:
        f.write(data)
    print("orig_len", orig_len, "hooks_bin_len", len(hooks_bin), "new_len", len(data))
    print("written:", dst)


if __name__ == "__main__":
    main()
