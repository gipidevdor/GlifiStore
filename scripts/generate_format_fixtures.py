#!/usr/bin/env python3
"""Generate or independently verify GlifiStore v1 format fixtures."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MANIFEST_MAGIC = 0x4D594C47
MANIFEST_VERSION = 1
MANIFEST_HEADER_BYTES = 128
MANIFEST_SEGMENT_ENTRY_BYTES = 32
MANIFEST_CHECKSUM_OFFSET = 80

COMPACTION_INTENT_MAGIC = 0x43594C47
COMPACTION_INTENT_VERSION = 1
COMPACTION_INTENT_HEADER_BYTES = 128
COMPACTION_INTENT_CHECKSUM_OFFSET = 56

SEGMENT_HEADER_MAGIC = 0x48594C47
SEGMENT_COMMIT_MAGIC = 0x43594C47
SEGMENT_HEADER_VERSION = 1
SEGMENT_COMMIT_VERSION = 1
SEGMENT_HEADER_RESERVED_BYTES = 4096
SEGMENT_COMMIT_SLOT_BYTES = 128
SEGMENT_COMMIT_SLOTS_OFFSET = 128
SEGMENT_HEADER_FIXTURE_BYTES = SEGMENT_COMMIT_SLOTS_OFFSET + 2 * SEGMENT_COMMIT_SLOT_BYTES
SEGMENT_IMMUTABLE_CHECKSUM_OFFSET = 52

RECORD_MAGIC = 0x52594C47
RECORD_VERSION = 1
RECORD_HEADER_SIZE = 56
RECORD_ALIGNMENT = 8


def crc32c(data: bytes) -> int:
    poly = 0x82F63B78
    table = []
    for index in range(256):
        crc = index
        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = (crc >> 1) ^ (poly & mask)
        table.append(crc & 0xFFFFFFFF)
    crc = 0xFFFFFFFF
    for byte in data:
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return (~crc) & 0xFFFFFFFF


def crc32c_with_zeroed_checksum_field(data: bytes, checksum_offset: int = 20) -> int:
    return crc32c(data[:checksum_offset] + b"\x00\x00\x00\x00" + data[checksum_offset + 4 :])


def read_hex(path: Path) -> bytes:
    digits = []
    for token in path.read_text(encoding="utf-8").split():
        digits.append(int(token, 16))
    return bytes(digits)


def write_hex(path: Path, payload: bytes) -> None:
    lines = []
    for index in range(0, len(payload), 16):
        chunk = payload[index : index + 16]
        lines.append(" ".join(f"{byte:02x}" for byte in chunk))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def encode_manifest(
    store_id: bytes,
    manifest_generation: int,
    worker_count: int,
    routing_epoch: int,
    next_segment_id: int,
    next_segment_generation: int,
    segments: list[tuple[int, int, int, int]],
) -> bytes:
    total_size = MANIFEST_HEADER_BYTES + len(segments) * MANIFEST_SEGMENT_ENTRY_BYTES
    header = bytearray(MANIFEST_HEADER_BYTES)
    struct.pack_into("<IHHII", header, 0, MANIFEST_MAGIC, MANIFEST_VERSION, MANIFEST_HEADER_BYTES, total_size, MANIFEST_SEGMENT_ENTRY_BYTES)
    header[16:32] = store_id
    struct.pack_into(
        "<QIIQIHHHH",
        header,
        32,
        manifest_generation,
        1,
        worker_count,
        routing_epoch,
        len(segments),
        1,
        1,
        1,
        1,
    )
    struct.pack_into("<QI", header, 68, next_segment_id, next_segment_generation)
    body = bytearray()
    for segment_id, generation, owner, role in segments:
        entry = bytearray(MANIFEST_SEGMENT_ENTRY_BYTES)
        struct.pack_into("<QIIH", entry, 0, segment_id, generation, owner, role)
        body.extend(entry)
    payload = bytearray(header) + body
    struct.pack_into("<I", payload, MANIFEST_CHECKSUM_OFFSET, crc32c(bytes(payload[:MANIFEST_CHECKSUM_OFFSET]) + b"\x00\x00\x00\x00" + bytes(payload[MANIFEST_CHECKSUM_OFFSET + 4 :])))
    return bytes(payload)


def manifest_fixture() -> bytes:
    return encode_manifest(
        store_id=bytes([0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]),
        manifest_generation=0x0102030405060708,
        worker_count=2,
        routing_epoch=0x1112131415161718,
        next_segment_id=4,
        next_segment_generation=1,
        segments=[
            (1, 1, 0, 2),
            (2, 1, 0, 1),
            (3, 2, 1, 1),
        ],
    )


def compaction_intent_fixture() -> bytes:
    store_id = bytes([0x61, 0x62, 0x63]) + bytes(13)
    old_manifest = encode_manifest(
        store_id=store_id,
        manifest_generation=9,
        worker_count=1,
        routing_epoch=7,
        next_segment_id=5,
        next_segment_generation=1,
        segments=[
            (1, 1, 0, 2),
            (2, 2, 0, 2),
            (3, 1, 0, 2),
            (4, 1, 0, 1),
        ],
    )
    next_manifest = encode_manifest(
        store_id=store_id,
        manifest_generation=10,
        worker_count=1,
        routing_epoch=7,
        next_segment_id=5,
        next_segment_generation=1,
        segments=[
            (1, 2, 0, 2),
            (4, 1, 0, 1),
        ],
    )
    total_size = COMPACTION_INTENT_HEADER_BYTES + len(old_manifest) + len(next_manifest)
    payload = bytearray(total_size)
    struct.pack_into(
        "<IHHII",
        payload,
        0,
        COMPACTION_INTENT_MAGIC,
        COMPACTION_INTENT_VERSION,
        COMPACTION_INTENT_HEADER_BYTES,
        total_size,
        0,
    )
    payload[16:32] = store_id
    struct.pack_into("<QQII", payload, 32, 9, 10, len(old_manifest), len(next_manifest))
    payload[COMPACTION_INTENT_HEADER_BYTES : COMPACTION_INTENT_HEADER_BYTES + len(old_manifest)] = old_manifest
    payload[COMPACTION_INTENT_HEADER_BYTES + len(old_manifest) :] = next_manifest
    struct.pack_into(
        "<I",
        payload,
        COMPACTION_INTENT_CHECKSUM_OFFSET,
        crc32c_with_zeroed_checksum_field(bytes(payload), COMPACTION_INTENT_CHECKSUM_OFFSET),
    )
    return bytes(payload)


def segment_commit_slot(generation: int, committed_end: int, state: int, record_count: int, first_seq: int, last_seq: int) -> bytes:
    slot = bytearray(SEGMENT_COMMIT_SLOT_BYTES)
    struct.pack_into("<IHHQ IHH QQQ", slot, 0, SEGMENT_COMMIT_MAGIC, SEGMENT_COMMIT_VERSION, SEGMENT_COMMIT_SLOT_BYTES, generation, committed_end, state, 0, record_count, first_seq, last_seq)
    struct.pack_into("<I", slot, 48, crc32c_with_zeroed_checksum_field(bytes(slot), 48))
    return bytes(slot)


def segment_header_fixture() -> bytes:
    header = bytearray(SEGMENT_HEADER_FIXTURE_BYTES)
    store_id = bytes([0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
    struct.pack_into("<IHHHHI", header, 0, SEGMENT_HEADER_MAGIC, SEGMENT_HEADER_VERSION, 128, 1, 1, SEGMENT_HEADER_RESERVED_BYTES)
    header[16:32] = store_id
    struct.pack_into("<QII", header, 32, 0x0102030405060708, 0x11223344, 0x55667788)
    struct.pack_into(
        "<I",
        header,
        SEGMENT_IMMUTABLE_CHECKSUM_OFFSET,
        crc32c_with_zeroed_checksum_field(bytes(header[:128]), SEGMENT_IMMUTABLE_CHECKSUM_OFFSET),
    )
    empty = segment_commit_slot(1, SEGMENT_HEADER_RESERVED_BYTES, 1, 0, 0, 0)
    populated = segment_commit_slot(2, 0x1100, 2, 2, 10, 11)
    header[SEGMENT_COMMIT_SLOTS_OFFSET : SEGMENT_COMMIT_SLOTS_OFFSET + SEGMENT_COMMIT_SLOT_BYTES] = empty
    header[SEGMENT_COMMIT_SLOTS_OFFSET + SEGMENT_COMMIT_SLOT_BYTES : SEGMENT_COMMIT_SLOTS_OFFSET + 2 * SEGMENT_COMMIT_SLOT_BYTES] = populated
    return bytes(header)


def record_fixture() -> bytes:
    key = bytes([0x00, 0x6B, 0xFF])
    value = bytes([0x10, 0x20, 0x30, 0x40])
    total_size = RECORD_HEADER_SIZE + len(key) + len(value)
    total_size = ((total_size + RECORD_ALIGNMENT - 1) // RECORD_ALIGNMENT) * RECORD_ALIGNMENT
    payload = bytearray(total_size)
    struct.pack_into(
        "<IHHIIIIQQQHHI",
        payload,
        0,
        RECORD_MAGIC,
        RECORD_VERSION,
        RECORD_HEADER_SIZE,
        total_size,
        len(key),
        len(value),
        0,
        0x0102030405060708,
        0x1112131415161718,
        0x2122232425262728,
        1,
        1,
        0x11223344,
    )
    payload[RECORD_HEADER_SIZE : RECORD_HEADER_SIZE + len(key)] = key
    payload[RECORD_HEADER_SIZE + len(key) : RECORD_HEADER_SIZE + len(key) + len(value)] = value
    struct.pack_into("<I", payload, 20, crc32c_with_zeroed_checksum_field(bytes(payload)))
    return bytes(payload)


def verify_manifest(data: bytes) -> None:
    if len(data) < MANIFEST_HEADER_BYTES:
        raise ValueError("manifest fixture too short")
    magic, version, header_bytes, total_size, entry_bytes, _ = struct.unpack_from("<IHHIIH", data, 0)
    if (magic, version, header_bytes, entry_bytes) != (MANIFEST_MAGIC, MANIFEST_VERSION, MANIFEST_HEADER_BYTES, MANIFEST_SEGMENT_ENTRY_BYTES):
        raise ValueError("manifest header fields mismatch")
    if len(data) != total_size:
        raise ValueError("manifest total size mismatch")
    checksum = struct.unpack_from("<I", data, MANIFEST_CHECKSUM_OFFSET)[0]
    if checksum != crc32c(data[:MANIFEST_CHECKSUM_OFFSET] + b"\x00\x00\x00\x00" + data[MANIFEST_CHECKSUM_OFFSET + 4 :]):
        raise ValueError("manifest checksum mismatch")


def verify_compaction_intent(data: bytes) -> None:
    if len(data) < COMPACTION_INTENT_HEADER_BYTES:
        raise ValueError("compaction intent fixture too short")
    magic, version, header_bytes, total_size, worker_id = struct.unpack_from("<IHHII", data, 0)
    if (magic, version, header_bytes, worker_id) != (
        COMPACTION_INTENT_MAGIC,
        COMPACTION_INTENT_VERSION,
        COMPACTION_INTENT_HEADER_BYTES,
        0,
    ):
        raise ValueError("compaction intent header fields mismatch")
    if len(data) != total_size:
        raise ValueError("compaction intent total size mismatch")
    if any(data[60:COMPACTION_INTENT_HEADER_BYTES]):
        raise ValueError("compaction intent reserved bytes are nonzero")
    checksum = struct.unpack_from("<I", data, COMPACTION_INTENT_CHECKSUM_OFFSET)[0]
    if checksum != crc32c_with_zeroed_checksum_field(data, COMPACTION_INTENT_CHECKSUM_OFFSET):
        raise ValueError("compaction intent checksum mismatch")

    old_generation, next_generation, old_size, next_size = struct.unpack_from("<QQII", data, 32)
    if total_size != COMPACTION_INTENT_HEADER_BYTES + old_size + next_size:
        raise ValueError("compaction intent manifest extents mismatch")
    old_manifest = data[COMPACTION_INTENT_HEADER_BYTES : COMPACTION_INTENT_HEADER_BYTES + old_size]
    next_manifest = data[COMPACTION_INTENT_HEADER_BYTES + old_size :]
    verify_manifest(old_manifest)
    verify_manifest(next_manifest)
    if data[16:32] != old_manifest[16:32] or data[16:32] != next_manifest[16:32]:
        raise ValueError("compaction intent Store ID does not bind both manifests")
    if old_generation != struct.unpack_from("<Q", old_manifest, 32)[0]:
        raise ValueError("compaction intent old generation mismatch")
    if next_generation != struct.unpack_from("<Q", next_manifest, 32)[0] or next_generation != old_generation + 1:
        raise ValueError("compaction intent next generation mismatch")
    old_routing, old_workers, old_epoch = struct.unpack_from("<IIQ", old_manifest, 40)
    next_routing, next_workers, next_epoch = struct.unpack_from("<IIQ", next_manifest, 40)
    old_next_segment_id, old_next_segment_generation = struct.unpack_from("<QI", old_manifest, 68)
    next_next_segment_id, next_next_segment_generation = struct.unpack_from("<QI", next_manifest, 68)
    if worker_id >= old_workers:
        raise ValueError("compaction intent Worker ID is outside the manifest Worker set")
    if (
        (old_routing, old_workers, old_epoch, old_next_segment_id, old_next_segment_generation)
        != (next_routing, next_workers, next_epoch, next_next_segment_id, next_next_segment_generation)
    ):
        raise ValueError("compaction intent changes immutable manifest metadata")

    old_segments = [
        struct.unpack_from("<QIIH", old_manifest, MANIFEST_HEADER_BYTES + index * MANIFEST_SEGMENT_ENTRY_BYTES)
        for index in range(struct.unpack_from("<I", old_manifest, 56)[0])
    ]
    next_segments = [
        struct.unpack_from("<QIIH", next_manifest, MANIFEST_HEADER_BYTES + index * MANIFEST_SEGMENT_ENTRY_BYTES)
        for index in range(struct.unpack_from("<I", next_manifest, 56)[0])
    ]
    sealed_sources = [entry for entry in old_segments if entry[2] == worker_id and entry[3] == 2]
    retained = [entry for entry in old_segments if entry[2] != worker_id or entry[3] != 2]
    output_count = len(next_segments) - len(retained)
    if not sealed_sources or output_count < 0 or output_count > len(sealed_sources):
        raise ValueError("compaction intent source or replacement count is invalid")
    replacements = [(segment_id, generation + 1, owner, role) for segment_id, generation, owner, role in sealed_sources[:output_count]]
    expected_next = sorted(retained + replacements)
    if next_segments != expected_next:
        raise ValueError("compaction intent is not the canonical sealed-set replacement")


def verify_record(data: bytes) -> None:
    magic, version, header_size, total_size = struct.unpack_from("<IHH I", data, 0)
    if (magic, version, header_size) != (RECORD_MAGIC, RECORD_VERSION, RECORD_HEADER_SIZE):
        raise ValueError("record header mismatch")
    if len(data) != total_size or total_size % RECORD_ALIGNMENT != 0:
        raise ValueError("record extent mismatch")
    checksum = struct.unpack_from("<I", data, 20)[0]
    if checksum != crc32c_with_zeroed_checksum_field(data):
        raise ValueError("record checksum mismatch")


def verify_segment_header(data: bytes) -> None:
    if len(data) != SEGMENT_HEADER_FIXTURE_BYTES:
        raise ValueError("segment header fixture must contain the immutable header and both commit slots")
    magic, version, immutable_bytes, segment_version, record_version, reserved = struct.unpack_from("<IHHHHI", data, 0)
    if magic != SEGMENT_HEADER_MAGIC or version != SEGMENT_HEADER_VERSION or immutable_bytes != 128:
        raise ValueError("segment header prefix mismatch")
    if segment_version != 1 or record_version != 1 or reserved != SEGMENT_HEADER_RESERVED_BYTES:
        raise ValueError("segment header format fields mismatch")
    checksum = struct.unpack_from("<I", data, SEGMENT_IMMUTABLE_CHECKSUM_OFFSET)[0]
    if checksum != crc32c_with_zeroed_checksum_field(data[:128], SEGMENT_IMMUTABLE_CHECKSUM_OFFSET):
        raise ValueError("segment immutable checksum mismatch")
    for slot_index in range(2):
        offset = SEGMENT_COMMIT_SLOTS_OFFSET + slot_index * SEGMENT_COMMIT_SLOT_BYTES
        slot = data[offset : offset + SEGMENT_COMMIT_SLOT_BYTES]
        if slot[:4] == b"\x00\x00\x00\x00":
            continue
        if struct.unpack_from("<I", slot, 0)[0] != SEGMENT_COMMIT_MAGIC:
            raise ValueError("commit slot magic mismatch")
        checksum = struct.unpack_from("<I", slot, 48)[0]
        if checksum != crc32c_with_zeroed_checksum_field(slot, 48):
            raise ValueError("commit slot checksum mismatch")


def verify_directory(directory: Path) -> None:
    verify_manifest(read_hex(directory / "manifest_v1.hex"))
    verify_compaction_intent(read_hex(directory / "compaction_intent_v1.hex"))
    verify_segment_header(read_hex(directory / "segment_header_v1.hex"))
    verify_segment_header(read_hex(directory / "segment_v1_header.hex"))
    verify_record(read_hex(directory / "record_v1.hex"))


def generate_directory(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    write_hex(directory / "manifest_v1.hex", manifest_fixture())
    write_hex(directory / "compaction_intent_v1.hex", compaction_intent_fixture())
    write_hex(directory / "segment_header_v1.hex", segment_header_fixture())
    write_hex(directory / "segment_v1_header.hex", segment_header_fixture())
    write_hex(directory / "record_v1.hex", record_fixture())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="Write generated fixtures to this directory.")
    parser.add_argument("--verify", type=Path, help="Independently verify fixtures in this directory.")
    args = parser.parse_args()
    if args.output_dir:
        generate_directory(args.output_dir)
        verify_directory(args.output_dir)
        print(f"Generated and verified fixtures in {args.output_dir}")
    if args.verify:
        verify_directory(args.verify)
        print(f"Verified fixtures in {args.verify}")
    if not args.output_dir and not args.verify:
        parser.error("Specify --output-dir and/or --verify")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
