"""Check the built C3 image and history reserve."""

import hashlib
from pathlib import Path
import struct
import sys


def check_image(build):
    image = (build / "firmware.bin").read_bytes()
    partitions = (build / "partitions.bin").read_bytes()
    if len(image) < 24 or image[0] != 0xE9 or struct.unpack_from("<H", image, 12)[0] != 5:
        raise ValueError("Expected an ESP32-C3 application image")
    sizes = []
    for offset in range(0, len(partitions) - 31, 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from(
            "<2sBBLL16sL", partitions, offset)
        if magic == b"\xaa\x50" and kind == 0 and subtype in (0x10, 0x11):
            sizes.append(size)
    if len(sizes) != 2:
        raise ValueError("Expected two OTA app slots")
    occupied = (len(image) + 4095) & ~4095
    limit = min(sizes) - 256 * 1024
    if occupied > limit:
        raise ValueError("Firmware overlaps the 256 KiB history reserve")
    print(f"Image: {len(image):,} bytes; OTA slot: {min(sizes):,} bytes")
    print(f"Free before history reserve: {limit - occupied:,} bytes (erase-aligned)")
    print(f"SHA256: {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    check_image(Path(sys.argv[1]))
