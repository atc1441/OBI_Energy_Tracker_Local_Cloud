import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "reader_stock_v57.bin"
DST = HERE / "reader_modded_89mock_v90_ISKRA_MT691.bin"
HOOKS_BIN = HERE / "build" / "hooks.bin"     # same plain build as splice.py -- this variant only needs
HOOKS_SYM = HERE / "build" / "hooks.sym"     # the int24 fix (entry_int24), no FIX_NEGATIVE_POWER hooks
BASE = 0x4000

# ISKRA MT-691 variant (2026-07-11). UNVERIFIED ON REAL HARDWARE -- built entirely from a static-analysis
# fix based on a user-supplied SML capture from a third party; no live meter or sniffer was available to
# this session to confirm the fix in practice. Anyone flashing this should treat it as best-effort until
# confirmed working against a real ISKRA MT-691 (or report back if it doesn't help).
#
# THE BUG: a user reported that with an ISKRA MT-691 meter, NOTHING is decoded/displayed at all (not a
# sign/magnitude bug like the DWSB20.2TH one -- total silence). A raw SML capture from the meter was
# manually decoded and confirmed to be fully standards-compliant SML with no encoder defects of its own:
# a clean SML_GetList.Res with 5 list entries (manufacturer-ID "ISK", serial, then OBIS 1-0:1.8.0 (import,
# Unsigned32 + scaler -1), 1-0:2.8.0 (export, same), 1-0:16.7.0 (power, Integer16 with a CORRECTLY signed
# value -417, unlike the DWSB bug). Every value type involved (Unsigned32/Integer16/OctetString) is already
# supported by the stock SML value-type dispatch (sub_C0A4), and the generic OBIS matcher (sub_B0C8) walks
# variable-length fields correctly regardless of this meter's unusual leading manufacturer-ID/serial
# entries -- so nothing about the METER's telegram is at fault here, unlike the DWSB case.
#
# ROOT CAUSE (fully traced via IDA decompile/disasm, addresses verified against the actual binary, not
# assumed from names): sub_9DA0 -- the import/export multi-tariff (OBIS 1.8.0-1.8.4 / 2.8.0-2.8.4) cache/
# dedup helper called from sub_77B4 -- sets a 1-byte "suppress" flag at struct+0x10 (import) / +0x11
# (export) of unk_20000D68 whenever a tariff register that was present in the PREVIOUS telegram is MISSING
# from the current one, within a 30-minute (0x1B7740 ms) window. This is intended as a "don't trust a
# register that just vanished" dedup signal, and sub_9DA0 ALREADY correctly implements it at the value
# level: it forces the corresponding value field itself to the 0x7FFFFFFF sentinel when this fires, and
# the downstream reporting function sub_9D34 already no-ops cleanly on that sentinel (confirmed at
# 0x9D3E-0x9D46). So per-field suppression is already fully and correctly handled BEFORE this fix.
#
# The actual bug is one level up, in the outer dispatcher sub_AA7C (0xAAEE-0xAAFD): it ALSO reads those
# same two suppress flags and, if EITHER one is set, discards the ENTIRE decoded record by zeroing the
# struct pointer -- including the power value (decoded independently by unrelated function sub_9ED8) and
# whichever of import/export ISN'T flagged, plus the "processed OK" status write. This is redundant
# over-reach on top of logic that already worked correctly one level down, and it's disproportionately
# damaging for a meter like the MT-691 that reports only ONE tariff register (1-0:1.8.0, no 1.8.1-1.8.4):
# a single transient IR/parse miss of that one register (plausible given known flaky IR reception on this
# reader hardware, see reader-v38-meter-debug notes) sets the flag and then blanks EVERYTHING -- power
# included -- for up to 30 minutes, even though the SAME telegram's power field decoded just fine.
#
# THE FIX: delete the veto block entirely (pure removal, no replacement logic needed, since suppression
# already happens correctly downstream in sub_9D34). Verified via xrefs that nothing branches into the
# middle of this 8-instruction block from elsewhere in the binary, so it's safe to remove as a unit; the
# separate, legitimate "sub_77B4 returned NULL" check right after it (0xAB14) is untouched. Does not touch
# sub_9DA0/sub_B348/sub_C310 at all, so the 30-minute dedup/cache mechanism for genuine multi-tariff
# meters (which DO use 1.8.0-1.8.4) is fully preserved -- a real "register vanished for <30 min" event
# still correctly zeroes just that one value via the existing sentinel path, same as before this patch.
#
#   0xAAEE  LDRB R0, [R4,#0x10]   \
#   0xAAF0  CMP  R0, #1           |
#   0xAAF2  BEQ  loc_AAFA         |  read/branch on the import suppress flag
#   0xAAF4  LDRB R0, [R4,#0x11]   |
#   0xAAF6  CMP  R0, #1           |  read/branch on the export suppress flag
#   0xAAF8  BNE  loc_AB14         |
#   0xAAFA  MOVS R4, #0           |  discard: zero the whole struct pointer
#   0xAAFC  B    loc_AB14         /
#
# CORRECTED (2026-07-11, after a second verification pass): plain NOPs here would NOT have been a no-op --
# 0xAAFE (immediately after this 16-byte block) is NOT padding, it's the live start of a DIFFERENT,
# already-used switch case (loc_AAFE, reached from a `BEQ loc_AAFE` earlier in the enclosing function for
# the "R5==2" case, which calls sub_7434 and overwrites R4 with its result before jumping on to loc_AB14
# itself). Falling through into that via plain NOPs would have unconditionally called sub_7434 with
# whatever's on the stack and clobbered R4 with its result on every single energy telegram -- a NEW, worse
# bug. Confirmed via disasm + xrefs_to that nothing else jumps into the middle of 0xAAEE-0xAAFD, so
# overwriting the whole 16-byte block is still safe -- it just needs to explicitly branch to the true merge
# point (0xAB14) instead of relying on fall-through.
#
# Patch: `B loc_AB14` (Thumb T2 unconditional branch, encoding 0xE000|imm11) as the first instruction, then
# 7x NOP filler. Verified against the ALREADY-PRESENT `B loc_AB14` at 0xAAFC (bytes "0A E0" -> halfword
# 0xE00A -> imm11=10 -> offset=20 -> target=(0xAAFC+4)+20=0xAB14, confirming the encoding convention this
# binary/toolchain uses) before computing the new one: source=0xAAEE, PC=source+4=0xAAF2,
# offset=0xAB14-0xAAF2=0x22(34, even), imm11=17=0x011, halfword=0xE000|0x011=0xE011 -> LE bytes "11 E0".
NOP_PATCH_ADDR = 0xAAEE
NOP_PATCH_LEN = 16
NOP_PATCH_EXPECT = bytes([
    0x20, 0x7C, 0x01, 0x28, 0x02, 0xD0, 0x60, 0x7C,
    0x01, 0x28, 0x0C, 0xD1, 0x00, 0x24, 0x0A, 0xE0,
])
NOP_INSTR = bytes([0xC0, 0x46])  # "MOV R8,R8" -- the standard Thumb NOP encoding
BRANCH_TO_AB14 = bytes([0x11, 0xE0])  # "B loc_AB14" from 0xAAEE -- see derivation above

HOOKS = [
    (0xC0EA, bytes([0x00, 0x20, 0xF8, 0xBD]), "entry_int24"),
    # SML TL=0x53 (Integer16) missing sign-extension fix (2026-07-11) -- see entry.S for the full writeup.
    # A general firmware defect, not specific to any one meter model, so it's in every variant's HOOKS
    # list -- but especially relevant here, since the ISKRA MT-691's power reading IS a plain Integer16
    # field (confirmed from the user-supplied SML capture this whole variant is based on) and negative
    # (-417 W in that capture): without this fix, even after the sub_AA7C discard-block fix below, a
    # negative ISKRA power reading would still come out wrong -- not flipped-sign, but reinterpreted as an
    # unsigned magnitude (e.g. -417 W -> +65119 W), same failure mode traced for this exact byte pattern.
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

    # the sub_AA7C veto-block removal (see comment block above)
    noff = NOP_PATCH_ADDR - BASE
    got = bytes(data[noff:noff + NOP_PATCH_LEN])
    assert got == NOP_PATCH_EXPECT, f"@{hex(NOP_PATCH_ADDR)}: expected {NOP_PATCH_EXPECT.hex()}, found {got.hex()} -- already patched or wrong base file?"
    patch_bytes = BRANCH_TO_AB14 + NOP_INSTR * ((NOP_PATCH_LEN - len(BRANCH_TO_AB14)) // len(NOP_INSTR))
    assert len(patch_bytes) == NOP_PATCH_LEN
    print(f"patch @{hex(NOP_PATCH_ADDR)}: B loc_AB14 + NOP fill (sub_AA7C veto-block removal)  bytes={patch_bytes.hex(' ')}")
    data[noff:noff + NOP_PATCH_LEN] = patch_bytes

    data += hooks_bin

    # softver 57 -> 89 / release named "v90" -- same permanent mock-version scheme as
    # reader_modded_89mock_v90.bin and reader_modded_89mock_v90_DWSB20_2TH.bin (see the comment above the
    # softver patch in either of those scripts for why): lets this file be re-uploaded/reflashed any
    # number of times without the OTA path's "already this version" no-op.
    for off in (0x8b36, 0x8b80):
        assert data[off] == 0x39 and data[off + 1] == 0x20, f"unexpected bytes at {hex(off)}: {data[off:off+2].hex()}"
        data[off] = 0x59  # 0x59 = 89 decimal (intentionally != the v90 in DST's filename)

    with open(DST, "wb") as f:
        f.write(data)
    print("orig_len", orig_len, "hooks_bin_len", len(hooks_bin), "new_len", len(data))
    print("written:", DST)


if __name__ == "__main__":
    main()
