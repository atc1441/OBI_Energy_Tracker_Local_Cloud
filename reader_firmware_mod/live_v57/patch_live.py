"""Make a live variant (~1 s readings) of a v57-based reader image.

Takes reader_stock_v57.bin or any reader_modded_89mock_v90*.bin from
reader_firmware_mod/ and writes the matching *_live_91mock_v92* file.

    ./build.sh
    python3 patch_live.py ../reader_modded_89mock_v90.bin
    python3 patch_live.py --all
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MOD = HERE.parent
BASE = 0x4000            # repo numbering: file offset + 0x4000
RUNTIME = 0x4800         # real load address of the app
FLASH_LIMIT = 0xFC00     # bootloader rejects images reaching this page

KNOWN = {
    "reader_stock_v57.bin": "f44c6cb4fff508576fba6441dcaeeba66e6f3c8d0e9141df071b058bf2518d21",
    "reader_modded_89mock_v90.bin": "437b9760fbed1ce912ca0967b6493026de91b464bd7f94219eb8488279f863d6",
    "reader_modded_89mock_v90_sf9.bin": "0ce6921f4fb61bf0ac80a7f4a0fe8d9b9d0c895aeeef10cc40b88a050ea06b6a",
    "reader_modded_89mock_v90_DWSB20_2TH.bin": "8f89910c3b89bf402f1eeaba77f78c6220925f2d4dfa4b55ec03f24ad072a659",
    "reader_modded_89mock_v90_DWSB20_2TH_sf9.bin": "5ddbc9732a7bafe298b1a69607f48f8c0be6e838e50241c51e1bb23a27b065d0",
    "reader_modded_89mock_v90_EMHeHZP.bin": "bd524458a267f3b5390aa67d5575965e20e408d318df36a8634f9331e8e43fa2",
    "reader_modded_89mock_v90_EMHeHZP_sf9.bin": "4470bd84c9849c377e3faed8784ffe7c5e39b94bb9f79a006e25b84f0dde0487",
}

# (address, stock bytes, hook): 4-byte sites redirected by BL
SITES = [
    (0xD11E, "fef753fb", "entry_cycle_due"),     # bl sub_B7C8, meter cycle wait
    (0xB942, "1f490020", "entry_window_base"),   # ldr r1,=9950; movs r0,#0
    (0xB906, "2e494018", "entry_window_retry"),  # ldr r1,=9950; adds r0,r0,r1
]
SOFTVER_SITES = (0xCB36, 0xCB80)          # energy/announce frame softver
SOFTVER_NEW = 91


def bl_encode(src, dst):
    off = dst - (src + 4)
    imm = (off // 2) & ((1 << 24) - 1)
    s = 1 if off < 0 else 0
    i1, i2 = (imm >> 23) & 1, (imm >> 22) & 1
    j1, j2 = 1 ^ i1 ^ s, 1 ^ i2 ^ s
    return struct.pack("<HH", 0xF000 | (s << 10) | ((imm >> 11) & 0x3FF),
                       0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | (imm & 0x7FF))


def live_name(name):
    if name == "reader_stock_v57.bin":
        return "reader_live_91mock_v92.bin"
    return name.replace("_89mock_v90", "_live_91mock_v92")


def load_offsets():
    text = (HERE / "build" / "live.sym").read_text()
    return {name: int(off, 16) for off, _, name in (line.split() for line in text.splitlines())}


def make_live(data):
    data = bytearray(data)
    hook = (HERE / "build" / "live.bin").read_bytes()
    offsets = load_offsets()
    data += b"\x00" * (-len(data) % 4)    # literal pool needs 4-byte alignment
    blob = BASE + len(data)
    data += hook
    assert RUNTIME + len(data) <= FLASH_LIMIT, "image would reach the last flash page"

    for addr, orig, sym in SITES:
        assert data[addr - BASE:addr - BASE + 4].hex() == orig, f"unexpected bytes at {addr:#x}"
        data[addr - BASE:addr - BASE + 4] = bl_encode(addr, blob + offsets[sym])

    for addr in SOFTVER_SITES:
        assert data[addr - BASE] in (57, 89), f"unexpected softver at {addr:#x}"
        data[addr - BASE] = SOFTVER_NEW
    return bytes(data)


def patch_file(src, out_dir):
    data = src.read_bytes()
    if KNOWN.get(src.name) != hashlib.sha256(data).hexdigest():
        sys.exit(f"{src.name}: not a known v57/v90 release image")
    out = out_dir / live_name(src.name)
    out.write_bytes(make_live(data))
    print(f"written: {out.name} ({out.stat().st_size} B)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="*", type=Path, help="v57/v90 image(s)")
    ap.add_argument("--all", action="store_true", help="patch every known image in reader_firmware_mod/")
    ap.add_argument("-o", "--out-dir", type=Path, default=HERE, help="output directory")
    args = ap.parse_args()
    inputs = [MOD / n for n in KNOWN] if args.all else args.inputs
    if not inputs:
        ap.error("give image(s) or --all")
    for src in inputs:
        patch_file(src, args.out_dir)


if __name__ == "__main__":
    main()
