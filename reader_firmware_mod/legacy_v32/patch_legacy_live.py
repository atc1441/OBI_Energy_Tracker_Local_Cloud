"""Build the legacy v32 live-read reader image.

Input is your own stock v32 reader image, or anything that contains it:
a stock bridge 1.0.x app partition or a full bridge flash dump (the image
is embedded there). No vendor binary ships with this repo.

    ./build.sh
    python3 patch_legacy_live.py <image-or-dump>
    python3 patch_legacy_live.py <image-or-dump> --extract-only
"""
import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = 0x4000            # repo numbering: file offset + 0x4000
RUNTIME = 0x4800         # real load address of the app
STOCK_SIZE = 46020
STOCK_SHA256 = "4dfb9c9444d7413fc225cbdc5c275f1688c3308e874d8bee3d53a93360beb8e2"
OUT_LIVE = "reader_legacy_live_92mock_v93.bin"
OUT_STOCK = "reader_legacy_stock_v32.bin"

# (address, stock bytes) of the one patched call
CALL_SITE = (0xD7DE, "fef749fc")          # bl 0xC074, meter cycle wait
SOFTVER_SITES = (0xD244, 0xAFE8)          # frame byte, own-version check
SOFTVER_OLD, SOFTVER_NEW = 0x20, 0x5C     # 32 -> 92


def bl_encode(src, dst):
    off = dst - (src + 4)
    imm = (off // 2) & ((1 << 24) - 1)
    s = 1 if off < 0 else 0
    i1, i2 = (imm >> 23) & 1, (imm >> 22) & 1
    j1, j2 = 1 ^ i1 ^ s, 1 ^ i2 ^ s
    return struct.pack("<HH", 0xF000 | (s << 10) | ((imm >> 11) & 0x3FF),
                       0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | (imm & 0x7FF))


def image_end(blob, start):
    """Image length from the scatter-load table."""
    words = len(blob[start:start + 0x10000]) // 4
    w = struct.unpack_from(f"<{words}I", blob, start)
    for k in range(words - 5):
        src1, dst1, size1, _, src2, dst2 = w[k:k + 6]
        if dst1 == 0x20000000 and dst2 == dst1 + size1 and RUNTIME < src1 < src2:
            return src2 - RUNTIME
    return None


def find_stock(blob):
    """Locate the stock v32 image in blob."""
    others = []
    for off in range(0, len(blob) - 8, 4):
        sp, reset = struct.unpack_from("<II", blob, off)
        if reset != 0x48D9 or not 0x20000800 <= sp <= 0x20002000:
            continue
        end = image_end(blob, off)
        if end is None:
            continue
        img = bytes(blob[off:off + end])
        digest = hashlib.sha256(img).hexdigest()
        if digest == STOCK_SHA256:
            return img, others
        others.append(digest)
    return None, others


def load_symbols(path):
    text = path.read_text()
    return {m.group(2): int(m.group(1), 16)
            for m in re.finditer(r"^([0-9a-fA-F]+)\s+[Tt]\s+(\S+)$", text, re.M)}


def build_live(stock):
    data = bytearray(stock)
    hook = (HERE / "build" / "live.bin").read_bytes()
    sym = load_symbols(HERE / "build" / "live.sym")["entry_cycle_due"]
    assert sym - BASE + len(hook) <= 0xE774 - BASE, "hook exceeds the cave"
    data[sym - BASE:sym - BASE + len(hook)] = hook

    addr, orig = CALL_SITE
    assert data[addr - BASE:addr - BASE + 4].hex() == orig, f"unexpected bytes at {addr:#x}"
    data[addr - BASE:addr - BASE + 4] = bl_encode(addr, sym)

    for addr in SOFTVER_SITES:
        assert data[addr - BASE] == SOFTVER_OLD, f"unexpected softver at {addr:#x}"
        data[addr - BASE] = SOFTVER_NEW
    assert len(data) == STOCK_SIZE
    return bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", type=Path, help="stock v32 image, bridge partition or flash dump")
    ap.add_argument("--extract-only", action="store_true", help="write the stock image only")
    ap.add_argument("-o", "--out", type=Path, help="output file")
    args = ap.parse_args()

    stock, others = find_stock(args.input.read_bytes())
    if stock is None:
        for digest in others:
            print(f"reader image with unknown build: sha256 {digest}")
        sys.exit("No stock v32 reader image found in input.")

    if args.extract_only:
        out = args.out or HERE / OUT_STOCK
        out.write_bytes(stock)
    else:
        out = args.out or HERE / OUT_LIVE
        out.write_bytes(build_live(stock))
    data = out.read_bytes()
    print(f"written: {out} ({len(data)} B, sha256 {hashlib.sha256(data).hexdigest()})")


if __name__ == "__main__":
    main()
