#!/usr/bin/env python3
"""
obi_ble_codec.py — TEA frame codec for the OBI ENERGY TRACKER BLE link.

Reverse-engineered from the heyOBI app:
  - crypto  : hz0/C14033t3.java   (TEA, 128-bit key, 32 rounds, ECB, little-endian words)
  - frame   : jz0/C16270d0.java   (3-byte header + payload + zero-pad to %8)
  - fragment: hz0/C13938a3.java    (173-byte payload chunks, LAST flag, index, number)
  - payload : jz0/AbstractC16325v1 (UTF-8 JSON, polymorphic AppRequest/BridgeResponse)

Data flow:
  TX (App->Bridge, char ABF2):  JSON --utf8--> split(173) --> [hdr|payload|pad] --> TEA-enc --> write
  RX (Bridge->App, char ABF1):  notify --> TEA-dec --> parse frame --> reassemble --> JSON

The 16-byte TEA key is per-bridge and comes from the cloud:
  POST /bluetooth-challenges {btChallengeId} -> {"key": "<32 hex chars>"}

CLI examples:
  python obi_ble_codec.py selftest
  python obi_ble_codec.py encode --key 00112233445566778899AABBCCDDEEFF --data '{"type":"StatusRequest"}'
  python obi_ble_codec.py decode --key 00112233445566778899AABBCCDDEEFF <framehex> [<framehex> ...]
  python obi_ble_codec.py tea-enc --key <32hex> --block 0011223344556677
  python obi_ble_codec.py parse  <plaintext-frame-hex>        # inspect a decrypted frame
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import List, NamedTuple

# --------------------------------------------------------------------------- #
# TEA (Tiny Encryption Algorithm) — matches hz0/C14033t3
# --------------------------------------------------------------------------- #
MASK = 0xFFFFFFFF
DELTA = 0x9E3779B9              # golden-ratio constant
SUM0_DECRYPT = (DELTA * 32) & MASK  # 0xC6EF3720, initial sum for decryption
BLOCK = 8                      # 64-bit block
ROUNDS = 32


def _key_words(key: bytes) -> List[int]:
    if len(key) != 16:
        raise ValueError("TEA key must be exactly 16 bytes (32 hex chars)")
    return [int.from_bytes(key[i:i + 4], "little") for i in range(0, 16, 4)]


def _mix(y: int, s: int, ka: int, kb: int) -> int:
    # (((y<<4)+ka) ^ (y+sum)) ^ ((y>>>5)+kb)   — all mod 2^32
    return ((((y << 4) & MASK) + ka) & MASK) ^ ((y + s) & MASK) ^ (((y >> 5) + kb) & MASK)


def tea_encrypt_block(v0: int, v1: int, k: List[int]) -> tuple[int, int]:
    s = 0
    for _ in range(ROUNDS):
        s = (s + DELTA) & MASK
        v0 = (v0 + _mix(v1, s, k[0], k[1])) & MASK
        v1 = (v1 + _mix(v0, s, k[2], k[3])) & MASK
    return v0, v1


def tea_decrypt_block(v0: int, v1: int, k: List[int]) -> tuple[int, int]:
    s = SUM0_DECRYPT
    for _ in range(ROUNDS):
        v1 = (v1 - _mix(v0, s, k[2], k[3])) & MASK
        v0 = (v0 - _mix(v1, s, k[0], k[1])) & MASK
        s = (s - DELTA) & MASK
    return v0, v1


def tea_ecb(data: bytes, key: bytes, *, encrypt: bool) -> bytes:
    if len(data) % BLOCK != 0:
        raise ValueError(f"data length must be a multiple of {BLOCK} (got {len(data)})")
    k = _key_words(key)
    out = bytearray()
    fn = tea_encrypt_block if encrypt else tea_decrypt_block
    for i in range(0, len(data), BLOCK):
        v0 = int.from_bytes(data[i:i + 4], "little")
        v1 = int.from_bytes(data[i + 4:i + 8], "little")
        v0, v1 = fn(v0, v1, k)
        out += v0.to_bytes(4, "little") + v1.to_bytes(4, "little")
    return bytes(out)


# --------------------------------------------------------------------------- #
# Frame — matches jz0/C16270d0
# --------------------------------------------------------------------------- #
MAX_PAYLOAD = 173  # hz0/C13938a3.mo61106h chunk size


class Frame(NamedTuple):
    number: int   # message id, 0..126 (byte0 bits 0-6)
    index: int    # fragment index within the message (byte1)
    last: bool    # LAST flag (byte0 bit7)
    payload: bytes


def build_frame(number: int, index: int, last: bool, payload: bytes) -> bytes:
    """Plaintext frame: [flags|number][index][len][payload][0-pad to %8]."""
    if not (0 <= number <= 0x7F):
        raise ValueError("number must be 0..127")
    if not (0 <= index <= 0xFF):
        raise ValueError("index must be 0..255")
    if len(payload) > 0xFF:
        raise ValueError("payload too long for one frame (>255)")
    out = bytearray()
    out.append((number & 0x7F) | (0x80 if last else 0x00))
    out.append(index & 0xFF)
    out.append(len(payload) & 0xFF)
    out += payload
    while len(out) % BLOCK != 0:
        out.append(0x00)
    return bytes(out)


def parse_frame(data: bytes) -> Frame:
    """Parse a *decrypted* plaintext frame."""
    if len(data) < 3:
        raise ValueError("frame too short")
    number = data[0] & 0x7F
    last = bool(data[0] & 0x80)
    index = data[1] & 0xFF
    plen = data[2] & 0xFF
    payload = data[3:3 + plen]
    if len(payload) != plen:
        raise ValueError(f"declared payload len {plen} > available {len(payload)}")
    return Frame(number, index, last, payload)


# --------------------------------------------------------------------------- #
# Message <-> encrypted frames
# --------------------------------------------------------------------------- #
def encode_message(data: bytes, key: bytes, number: int = 0,
                   chunk_size: int = MAX_PAYLOAD) -> List[bytes]:
    """Full message bytes -> list of ENCRYPTED frames ready to write to ABF2.

    `chunk_size` (<= 173) is the payload bytes per fragment. Smaller fragments produce
    smaller BLE writes (useful when the negotiated ATT MTU is small); the device
    reassembles by index regardless of fragment size.
    """
    chunk_size = max(1, min(chunk_size, MAX_PAYLOAD))
    n = max(1, -(-len(data) // chunk_size))  # ceil
    frames = []
    for i in range(n):
        chunk = data[i * chunk_size:(i + 1) * chunk_size]
        plain = build_frame(number, i, i == n - 1, chunk)
        frames.append(tea_ecb(plain, key, encrypt=True))
    return frames


def decode_frames(enc_frames: List[bytes], key: bytes) -> bytes:
    """List of ENCRYPTED frames (as received on ABF1) -> reassembled message bytes."""
    parsed = [parse_frame(tea_ecb(f, key, encrypt=False)) for f in enc_frames]
    if not parsed:
        raise ValueError("no frames")
    number = parsed[0].number
    if any(p.number != number for p in parsed):
        raise ValueError("fragments must share the same packet number")
    parsed.sort(key=lambda p: p.index)
    if sum(1 for p in parsed if p.last) != 1 or not parsed[-1].last:
        raise ValueError("need exactly one LAST fragment, and it must be the last by index")
    return b"".join(p.payload for p in parsed)


def encode_request(obj, key: bytes, number: int = 0,
                   chunk_size: int = MAX_PAYLOAD) -> List[bytes]:
    """dict/str (AppRequest JSON) -> encrypted frames."""
    js = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
    return encode_message(js.encode("utf-8"), key, number, chunk_size)


def decode_response(enc_frames: List[bytes], key: bytes) -> str:
    """encrypted frames (BridgeResponse) -> JSON string."""
    return decode_frames(enc_frames, key).decode("utf-8")


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _hexb(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", "").replace(":", "").replace("\n", ""))


def _read_frames(args) -> List[bytes]:
    frames: List[str] = list(args.frames or [])
    if args.infile:
        with open(args.infile, encoding="utf-8") as f:
            frames += [ln.strip() for ln in f if ln.strip()]
    if not frames:
        sys.exit("no frames given (pass hex args or --in file with one frame per line)")
    return [_hexb(x) for x in frames]


def cmd_encode(args):
    key = _hexb(args.key)
    if args.hex:
        data = _hexb(args.hex)
    elif args.data is not None:
        data = args.data.encode("utf-8")
    elif args.data_file:
        data = open(args.data_file, "rb").read()
    else:
        data = sys.stdin.buffer.read()
    frames = encode_message(data, key, args.number)
    print(f"[i] message {len(data)} bytes -> {len(frames)} frame(s), number={args.number}")
    for i, fr in enumerate(frames):
        print(f"\nframe[{i}] ({len(fr)} bytes ciphertext):")
        print("  cipher : " + fr.hex())
    if args.plain:
        # show the plaintext frames too (pre-encryption) for debugging
        n = max(1, -(-len(data) // MAX_PAYLOAD))
        for i in range(n):
            chunk = data[i * MAX_PAYLOAD:(i + 1) * MAX_PAYLOAD]
            print(f"plain[{i}] : " + build_frame(args.number, i, i == n - 1, chunk).hex())


def cmd_decode(args):
    key = _hexb(args.key)
    enc = _read_frames(args)
    data = decode_frames(enc, key)
    txt = data.decode("utf-8", "replace")
    print(f"[i] {len(enc)} frame(s) -> {len(data)} bytes")
    try:
        print(json.dumps(json.loads(txt), indent=2, ensure_ascii=False))
    except Exception:
        print(txt)


def cmd_parse(args):
    for i, h in enumerate(args.frames):
        data = _hexb(h)
        if args.key:
            data = tea_ecb(data, _hexb(args.key), encrypt=False)
        fr = parse_frame(data)
        print(f"frame[{i}]: number={fr.number} index={fr.index} last={fr.last} "
              f"len={len(fr.payload)}")
        print("  payload(hex) : " + fr.payload.hex())
        try:
            print("  payload(utf8): " + fr.payload.decode("utf-8"))
        except Exception:
            pass


def cmd_tea(args, encrypt: bool):
    key = _hexb(args.key)
    block = _hexb(args.block)
    out = tea_ecb(block, key, encrypt=encrypt)
    print(out.hex())


def cmd_selftest(args):
    key = bytes.fromhex("00112233445566778899AABBCCDDEEFF")  # mock key from hz0/C13935a0
    # 1) TEA block round-trip
    blk = bytes.fromhex("0123456789abcdef")
    ct = tea_ecb(blk, key, encrypt=True)
    pt = tea_ecb(ct, key, encrypt=False)
    assert pt == blk, "TEA block round-trip failed"
    print(f"[ok] TEA block: {blk.hex()} -enc-> {ct.hex()} -dec-> {pt.hex()}")

    # 2) frame build/parse round-trip
    fr = build_frame(5, 0, True, b"hello")
    assert len(fr) % 8 == 0
    p = parse_frame(fr)
    assert (p.number, p.index, p.last, p.payload) == (5, 0, True, b"hello")
    print(f"[ok] frame: {fr.hex()} -> number={p.number} index={p.index} last={p.last} "
          f"payload={p.payload!r}")

    # 3) single-fragment message round-trip
    req = {"type": "StatusRequest"}
    frames = encode_request(req, key, number=0)
    back = decode_response(frames, key)
    assert json.loads(back) == req, "single-fragment round-trip failed"
    print(f"[ok] 1-fragment: {req} -> {len(frames)} frame -> {back}")

    # 4) multi-fragment message round-trip (payload > 173 bytes)
    big = {"type": "WifiSetRequest", "ssid": "X" * 200, "password": "p" * 120}
    frames = encode_request(big, key, number=7)
    assert len(frames) >= 2, "expected multiple fragments"
    back = json.loads(decode_response(frames, key))
    assert back == big, "multi-fragment round-trip failed"
    print(f"[ok] multi-fragment: {len(frames)} frames, number=7, reassembled OK")

    # 5) real captured frame from a live gateway (btsnoop, ABF2/write)
    real_key = bytes.fromhex("00112233445566778899AABBCCDDEEFF")
    real_cipher = bytes.fromhex(
        "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF")
    got = decode_response([real_cipher], real_key)
    assert json.loads(got) == {"type": "WifiScanRequest"}, got
    print(f"[ok] real captured frame decodes: {got}")
    print("\nAll self-tests passed.")


def build_parser():
    p = argparse.ArgumentParser(description="OBI ENERGY TRACKER BLE TEA frame codec.",
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("encode", help="JSON/bytes -> encrypted ABF2 frames")
    sp.add_argument("--key", required=True, help="16-byte TEA key (hex)")
    g = sp.add_mutually_exclusive_group()
    g.add_argument("--data", help="raw string payload (e.g. JSON)")
    g.add_argument("--data-file", help="read payload bytes from file")
    g.add_argument("--hex", help="payload as hex bytes")
    sp.add_argument("--number", type=int, default=0, help="packet number (0..126)")
    sp.add_argument("--plain", action="store_true", help="also print plaintext frames")
    sp.set_defaults(fn=cmd_encode)

    sp = sub.add_parser("decode", help="encrypted ABF1 frames -> JSON")
    sp.add_argument("--key", required=True, help="16-byte TEA key (hex)")
    sp.add_argument("frames", nargs="*", help="encrypted frame(s) as hex")
    sp.add_argument("--in", dest="infile", help="file with one frame (hex) per line")
    sp.set_defaults(fn=cmd_decode)

    sp = sub.add_parser("parse", help="inspect a frame (optionally decrypt first with --key)")
    sp.add_argument("frames", nargs="+", help="frame(s) as hex")
    sp.add_argument("--key", help="if given, TEA-decrypt before parsing")
    sp.set_defaults(fn=cmd_parse)

    sp = sub.add_parser("tea-enc", help="TEA-encrypt one/many 8-byte blocks (ECB)")
    sp.add_argument("--key", required=True); sp.add_argument("--block", required=True)
    sp.set_defaults(fn=lambda a: cmd_tea(a, True))

    sp = sub.add_parser("tea-dec", help="TEA-decrypt one/many 8-byte blocks (ECB)")
    sp.add_argument("--key", required=True); sp.add_argument("--block", required=True)
    sp.set_defaults(fn=lambda a: cmd_tea(a, False))

    sub.add_parser("selftest", help="run round-trip self-tests").set_defaults(fn=cmd_selftest)
    return p


def main():
    args = build_parser().parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
