import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_stock_v57.bin"
DST = HERE / "reader_modded_89mock_v90_DWSB20_2TH.bin"
HOOKS_BIN = HERE / "build" / "hooks_DWSB20_2TH.bin"
HOOKS_SYM = HERE / "build" / "hooks_DWSB20_2TH.sym"
BASE = 0x4000

# DWSB20.2TH-specific variant. Build with `./build.sh DWSB20_2TH` first
# (adds -DFIX_NEGATIVE_POWER for both hooks.c and entry.S).
#
# Confirmed live 2026-07-10: the bug is specifically in the SML Int24
# (TL=0x54) path -- NOT Int8/Int16 (an earlier revision of this script
# also patched the sub_C0A4 dispatch table for TL=0x52/0x53, based on a
# hypothesis about small-magnitude readings using a narrower encoding;
# the user confirmed live that's not what's happening here, so that patch
# was removed again).
#
# History of the actual fix's evolution (kept so it isn't re-tried blind):
#   1. hook_decode_int24 with an OBIS-backward-scan (fixed -16 offset,
#      then a wider 4-24 byte scan, then a tight unit/scaler-adjacent
#      check) to identify "this Int24 value is OBIS 16.7.0" from inside
#      the low-level SML decoder, before applying the correction. Each
#      variant was proven correct via a byte-for-byte Python replay
#      against one real meter_sniffer capture (-42.74 W reproduced
#      correctly from raw bytes "00 EF 4E"), but NONE fired reliably live,
#      even after ruling out stale-OTA caching by bumping to a fresh
#      never-used softver on every attempt. The exact byte layout
#      preceding the value field apparently isn't as constant as that one
#      capture suggested.
#   2. A second hook (now entry_power_correct, in hooks.c/entry.S) at
#      0x75EE inside sub_7434 -- reached only after sub_7434 already
#      matched the OBIS string "1-0:16.7.0*255" via sub_C000 a few
#      instructions earlier, so there's no ambiguity about which field
#      this is, no byte-scanning needed at all. This corrects the RAW
#      value (via the same import/export trend check that an even
#      earlier, abandoned attempt used) BEFORE sub_4700's scaler
#      conversion and the struct-fill, so the corrected value flows
#      through the exact same path any normal reading takes. (An earlier
#      attempt patched sub_7434's tail at 0x7604, AFTER the struct was
#      already filled -- that write never survived to the transmitted
#      energy payload, confirmed with a temporary gateway-side raw-payload
#      debug field; fixing the value while it's still mid-flow, here,
#      is the difference.)
HOOKS = [
    (0xC0EA, bytes([0x00, 0x20, 0xF8, 0xBD]), "entry_int24"),
    (0x75EE, bytes([0xFD, 0xF7, 0x87, 0xF8]), "entry_power_correct"),
    # sub_77B4/sub_9ED8 -- confirmed live to be the actually-active meter-
    # parser path on this reader (an unconditional sentinel at 0x75EE
    # above never showed up; this one's data did). Right after
    # `BL sub_9ED8` inside sub_77B4.
    (0x7800, bytes([0x06, 0x49, 0x08, 0x31]), "entry_power_correct2"),
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

    data += hooks_bin

    # softver 57 -> 89 -- deliberately does NOT match the "v90" release
    # name below. This is a "mock version" trick for the SHIPPED release
    # (not a temporary testing value, unlike earlier iterations of this
    # comment): the gateway's web UI parses the OTA target version from
    # the uploaded filename via /v(\d+)/ (see gateway_web.cpp, ~line 545)
    # and advertises THAT number to the reader; the reader only treats an
    # advertised version as "no update" if it equals 0 or its OWN
    # currently-reported softver. Since this file always reports 89
    # internally while its filename always advertises v90, those two
    # numbers can never coincide -- so this exact release file can be
    # re-uploaded/reflashed any number of times (e.g. to force a reader
    # back onto a known-good build) without ever hitting the "already
    # this version, skipping" OTA no-op that a same-numbered file would.
    for off in (0x8b36, 0x8b80):
        assert data[off] == 0x39 and data[off + 1] == 0x20, f"unexpected bytes at {hex(off)}: {data[off:off+2].hex()}"
        data[off] = 0x59  # 0x59 = 89 decimal (see comment above -- intentionally != the v90 in DST's filename)

    with open(DST, "wb") as f:
        f.write(data)
    print("orig_len", orig_len, "hooks_bin_len", len(hooks_bin), "new_len", len(data))
    print("written:", DST)


if __name__ == "__main__":
    main()
