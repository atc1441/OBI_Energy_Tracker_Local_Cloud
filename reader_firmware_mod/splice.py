import argparse
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_stock_v57.bin"
DST = HERE / "reader_modded_89mock_v90.bin"
DST_SF9 = HERE / "reader_modded_89mock_v90_sf9.bin"
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

# --sf9: LoRa spreading factor SF7 -> SF9 (2026-07-21). radio_init_config (starts 0xB69C) calls the vendor
# SX126x HAL twice -- RadioSetTxConfig and RadioSetRxConfig -- each taking the datarate as a plain immediate:
#   0xB6D8: `movs r0, #7` (bytes 07 20) -- datarate arg for RadioSetTxConfig (stack slot 0)
#   0xB700: `movs r2, #7` (bytes 07 22) -- datarate arg for RadioSetRxConfig (register r2)
# Both are single-byte immediates (Thumb MOVS Rd,#imm8 encodes the value directly in the low byte), no
# relocation needed. Verified by hand-disassembling radio_init_config and cross-checking every other
# stack/register argument at these two call sites against the already-reversed radio table in
# ../03-reverse-engineering/lora-protocol.md (bandwidth idx 2, coderate 1, preamble 12, TX power 22 dBm all
# land exactly where expected around these two instructions). Must be paired with the gateway's own SF
# setting (Settings page) -- a reader on SF9 can't hear an SF7 beacon (or vice versa), so flash this to the
# reader FIRST while the gateway is still on SF7 (same-SF link needed for the OTA transfer itself), THEN
# switch the gateway to SF9 once the reader has rebooted into it.
#
# The SF patch alone is NOT enough, live-confirmed: at SF9 the reader's own ECDH-reply wait (inside
# `ecdh_keyexchange_sm`, 0x66AE) times out before the gateway's reply ever lands -- it's hardcoded to 300 ms
# (`movs r3,#255` + `adds r3,#45` = 300, at 0x66D8-0x66DB), but the gateway's actual round-trip at SF9 is
# ~360 ms (255 ms turnaround + ~107 ms time-on-air for the 68-byte reply, vs. ~32 ms at SF7 -- computed from
# the real LoRa airtime formula at BW500). The reader retries 5 times then gives up and resets, matching the
# handshake livelock observed live. Fix: widen the `adds` immediate from 45 (0x2D) to 245 (0xF5) at 0x66DA --
# same single byte, new total 255+245=500 ms, ~140 ms of margin over the measured SF9 round-trip. Confirmed
# identical code+address in both `reader_stock_v57.bin` and a separately reversed `reader_v1.2.1.elf`
# (matching disassembly, matching 300 constant) -- this isn't a v57-specific guess.
#
# A THIRD, independent SF7 hardcode (2026-07-21, live-confirmed): the case-button "bind" announce kept
# transmitting at SF7 even after the two patches above, live-verified by watching the gateway (on SF9) go
# deaf to the reader the moment the button was pressed, then hear it again only after switching the gateway
# back to SF7. radio_init_config is NOT involved this time -- its only two callers (both traced) go through
# the same already-patched code. The actual second site is a small, otherwise-unnamed TX-config helper at
# 0x6C64-0x6C92 (IDA fails to recognize it as its own function -- it silently merges into the neighboring
# sub_6C2C/sub_6C80 -- found by hand-disassembling from a confirmed-good boundary), called from `sub_D370`
# (BL at 0xD3D4) which is itself invoked out of the energy-report/announce transmit path (`sub_5D04`). Same
# fix shape: `movs r1,#7` at 0x6C78 (bytes `07 21`) -> `movs r1,#9` (`09 21`), one byte, no relocation.
SF9_PATCHES = [(0xB6D8, 0x07, 0x09), (0xB700, 0x07, 0x09), (0x66DA, 0x2D, 0xF5), (0x6C78, 0x07, 0x09)]


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

    if args.sf9:
        for addr, old, new in SF9_PATCHES:
            off = addr - BASE
            assert data[off] == old, f"@{hex(addr)}: expected {old:#04x}, found {data[off]:#04x} -- already patched or wrong base file?"
            data[off] = new
            print(f"patch @{hex(addr)}: SF7->SF9 ({old:#04x} -> {new:#04x})")

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

    with open(dst, "wb") as f:
        f.write(data)
    print("orig_len", orig_len, "hooks_bin_len", len(hooks_bin), "new_len", len(data))
    print("written:", dst)


if __name__ == "__main__":
    main()
