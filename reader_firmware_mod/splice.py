import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_stock_v57.bin"
DST = HERE / "reader_modded_89mock_v90.bin"
HOOKS_BIN = HERE / "build" / "hooks.bin"
HOOKS_SYM = HERE / "build" / "hooks.sym"
BASE = 0x4000

# call sites to patch: (address, expected original 4 bytes, entry symbol name)
HOOKS = [
    (0xC0EA, bytes([0x00, 0x20, 0xF8, 0xBD]), "entry_int24"),
    # SML TL=0x53 (Integer16) missing sign-extension fix (2026-07-11) -- see entry.S for the full writeup.
    # Replaces `asrs r1,r0,#0x1f ; str r1,[r4,#4]` inside sub_C0A4's Int16 case; a general firmware defect,
    # not specific to any one meter model, so it's in every variant's HOOKS list.
    (0xC110, bytes([0xC1, 0x17, 0x61, 0x60]), "entry_sxth16_fix"),
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
                syms[m.group(2)] = int(m.group(1), 16) & ~1  # strip thumb bit if any
    return syms


def main():
    with open(SRC, "rb") as f:
        data = bytearray(f.read())
    orig_len = len(data)
    tramp_addr = BASE + orig_len

    with open(HOOKS_BIN, "rb") as f:
        hooks_bin = f.read()

    syms = load_symbols(HOOKS_SYM)
    print("symbols:", {k: hex(v) for k, v in syms.items()})

    # sanity: hooks.bin must have been linked to start exactly at tramp_addr
    # (link.ld's ORIGIN is hardcoded to match reader_meter_v57.bin's current
    # length -- if the base firmware ever changes size, link.ld must be
    # updated to match, or this whole scheme silently mis-links).
    assert tramp_addr == 0xEE08, f"base firmware length changed; update link.ld ORIGIN (expected 0xEE08, got {hex(tramp_addr)})"

    for addr, expect, sym in HOOKS:
        off = addr - BASE
        got = bytes(data[off:off + len(expect)])
        assert got == expect, f"@{hex(addr)}: expected {expect.hex()}, found {got.hex()} -- already patched or wrong base file?"
        target = syms[sym]
        patch = bl_encode(addr, target)
        print(f"patch @{hex(addr)}: BL -> {sym}@{hex(target)}  bytes={patch.hex(' ')}")
        data[off:off + len(patch)] = patch

    data += hooks_bin

    # softver 57 -> 89 -- deliberately does NOT match the "v90" release
    # name below (breaks byte-for-byte equivalence with the older,
    # hand-assembled reader_meter_v87_clean.bin reference build, in favor
    # of matching splice_DWSB20_2TH.py's mock-version numbers exactly, so
    # both release variants use the identical 89/"v90" pair).
    #
    # "89mock_v90" naming (2026-07-11, matching splice_DWSB20_2TH.py): the
    # firmware's OWN reported softver is 89, but the RELEASE is
    # named/tagged "v90" -- a permanent "mock version" mismatch, not a
    # leftover dev value. The gateway's web UI parses the OTA target
    # version to advertise from the uploaded filename via /v(\d+)/ (see
    # gateway_web.cpp, ~line 545) and only treats an advertised version as
    # a no-op if it equals 0 or the reader's own currently-reported
    # softver. Since 90 can never equal this file's own reported 89, this
    # exact release file can be re-uploaded/reflashed any number of times
    # (e.g. to force a reader back onto a known-good build) without ever
    # hitting the "already this version, skipping" OTA no-op a
    # same-numbered file would.
    for off in (0x8b36, 0x8b80):
        assert data[off] == 0x39 and data[off + 1] == 0x20, f"unexpected bytes at {hex(off)}: {data[off:off+2].hex()}"
        data[off] = 0x59  # 0x59 = 89 decimal (intentionally != the v90 in DST's filename)

    with open(DST, "wb") as f:
        f.write(data)
    print("orig_len", orig_len, "hooks_bin_len", len(hooks_bin), "new_len", len(data))
    print("written:", DST)


if __name__ == "__main__":
    main()
