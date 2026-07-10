import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_modded_v87.bin"
DST = HERE / "reader_modded_v88_negfix.bin"
BASE = 0x4000

# ---------------------------------------------------------------------------
# Second SML sign-extension bug, found by IDA-diffing sub_C0A4 (the generic
# SML value-type switch, same function the int24/TL=0x54 fix already lives
# in). This one affects TL=0x52 (Integer8) and TL=0x53 (Integer16):
#
#   case TL=0x52 (@0xC0EE) does `LDRB` (zero-extend) instead of `LDRSB`
#   (sign-extend), and hardcodes the high dword to 0.
#
#   case TL=0x53 (@0xC106) computes the 16-bit value correctly but is
#   missing a single `SXTH R0, R0` before sign-extending into the high
#   dword -- so `ASRS R1, R0, #0x1F` always reads bit 31 of the *32-bit*
#   zero-extended value (always 0), never bit 15 of the real 16-bit value.
#
# Both bugs are silent for positive values (0 <= v < 0x80/0x8000): the
# missing sign-extension is a no-op when the sign bit is already 0, so any
# meter whose values happen to fit those ranges as positive numbers reads
# fine. It only surfaces once a value goes negative and needs the *shorter*
# SML width -- which is exactly what happens on some DWSB20.2TH units: SML
# encodes each value in the shortest width that fits, so small-magnitude
# feed-in power (a small negative number) gets sent as Integer16 instead of
# the Integer24 the existing int24 fix handles, and trips this second bug.
# That's why positive power was already correct after the int24 fix, but
# negative (feed-in) power still came out as a large bogus positive number.
#
# Fix: the same switch statement already contains CORRECT sign-extending
# code for a same-width read -- case TL=0x62 (@0xC0F8, 1-byte, `LDRSB` +
# `ASRS`) and case TL=0x63 (@0xC118, 2-byte, with the `SXTH`). Both read
# the exact same byte offsets as the buggy TL=0x52/0x53 cases. So instead
# of hand-writing new decode code, this just retargets the two dispatch
# table entries to reuse that already-correct code -- a 2-byte patch.
#
# Dispatch table for cases TL=0x52..0x5A lives right after the
# `BL __rt_switch8` at 0xC0CC: one header byte, then 9 entries, each an
# unsigned byte = (target_addr - 0xC0D0) / 2 (table base = table start,
# IDA-confirmed against all 9 entries incl. the shared default slot).
TABLE_BASE = 0xC0D0
PATCHES = [
    # (table_addr, expect_byte, new_target, why)
    (0xC0D1, 0x0F, 0xC0F8, "TL=0x52 (Int8): route to the correct signed-byte case (was TL=0x62's code)"),
    (0xC0D2, 0x1B, 0xC118, "TL=0x53 (Int16): route to the correct signed-int16 case (was TL=0x63's code)"),
]


def main():
    data = bytearray(SRC.read_bytes())

    for table_addr, expect, target, why in PATCHES:
        off = table_addr - BASE
        got = data[off]
        assert got == expect, (
            f"@{hex(table_addr)}: expected {hex(expect)}, found {hex(got)} "
            f"-- already patched, or SRC isn't the expected reader_modded_v87.bin?"
        )
        new_val = (target - TABLE_BASE) // 2
        assert 0 <= new_val <= 0xFF
        print(f"patch table@{hex(table_addr)}: {hex(got)} -> {hex(new_val)}  (jump to {hex(target)}) -- {why}")
        data[off] = new_val

    # softver 87 -> 88, same two locations the int24 fix's splice.py bumped
    # 57 -> 87 at (binary byte, not ASCII -- second byte of each pair is
    # unrelated and left untouched).
    for off in (0x8b36, 0x8b80):
        assert data[off] == 0x57, f"unexpected softver byte at {hex(off)}: {hex(data[off])}"
        data[off] = 0x58

    DST.write_bytes(data)
    print("written:", DST, "len", len(data))


if __name__ == "__main__":
    main()
