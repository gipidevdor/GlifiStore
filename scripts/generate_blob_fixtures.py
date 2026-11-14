#!/usr/bin/env python3
"""Independent Python/struct/hashlib fixtures for ADR 0042, with no C++ codec calls."""

import argparse
import hashlib
from pathlib import Path
import struct


def crc32c(data):
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def checksum(data, offset):
    struct.pack_into("<I", data, offset, crc32c(data))
    return bytes(data)


def fixtures():
    store_id = bytes(range(16))
    blob_id = bytes([1]) + bytes(15)
    payload = b"A" * (1 << 20) + b"xyz"
    digest = hashlib.sha256(payload).digest()
    ref = bytearray(80)
    struct.pack_into("<4sHH16sQIIH", ref, 0, b"GLBR", 1, 80, blob_id, len(payload), 1 << 20, 2, 1)
    ref[48:80] = digest
    header = bytearray(160)
    struct.pack_into("<4sHH16s16sIIQIHHQQQQ", header, 0, b"GLYB", 1, 128, blob_id,
                     store_id, 0, 1 << 20, len(payload), 2, 16, 1, 128, 160,
                     160 + len(payload), 224 + len(payload))
    struct.pack_into("<QIIQII", header, 128, 160, 1 << 20, crc32c(payload[:1 << 20]),
                     160 + (1 << 20), 3, crc32c(b"xyz"))
    footer = bytearray(64)
    struct.pack_into("<4sHHQ32s", footer, 0, b"GLBF", 1, 64, len(payload), digest)
    key = b"blob-key"
    key_hash = 14695981039346656037
    for byte in key:
        key_hash = ((key_hash ^ byte) * 1099511628211) & ((1 << 64) - 1)
    record = bytearray(144)
    struct.pack_into("<4sHHIIIIQQQHHI", record, 0, b"GLYR", 2, 56, 144, len(key), 80,
                     0, 7, key_hash, 0, 1, 5, 0)
    record[56:64], record[64:144] = key, ref
    manifest = bytearray(160)
    struct.pack_into("<4sHHIHH16sQIIQIHHHHQI", manifest, 0, b"GLYM", 2, 128, 160,
                     32, 0, store_id, 3, 1, 1, 1, 1, 1, 2, 2, 1, 2, 1)
    manifest[92] = 2
    struct.pack_into("<QHHHH", manifest, 108, 1, 1, 1, 1, 20)
    struct.pack_into("<QIIH", manifest, 128, 1, 1, 0, 1)
    segment = bytearray(128)
    struct.pack_into("<4sHHHHI16sQIII", segment, 0, b"GLYH", 2, 128, 1, 2, 4096,
                     store_id, 1, 1, 0, 0)
    return {
        "blob_ref_v1.hex": bytes(ref),
        "blob_file_v1_metadata.hex": checksum(header, 96),
        "blob_file_v1_footer.hex": checksum(footer, 48),
        "record_v2_blob_ref.hex": checksum(record, 20),
        "manifest_v2_blob.hex": checksum(manifest, 80),
        "segment_header_v2_prefix.hex": checksum(segment, 52),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    assert crc32c(b"123456789") == 0xE3069283
    root = Path(__file__).resolve().parents[1] / "tests" / "fixtures"
    for name, value in fixtures().items():
        target = root / name
        text = "\n".join(value.hex()[i:i + 64] for i in range(0, len(value) * 2, 64)) + "\n"
        if args.check:
            if not target.is_file() or target.read_text() != text:
                raise SystemExit(f"fixture differs: {target}")
        else:
            target.write_text(text)
    print("Blob fixtures match independent Python encoding" if args.check else "Wrote six blob/v2 fixtures")


if __name__ == "__main__":
    main()
