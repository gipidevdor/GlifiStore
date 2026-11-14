#!/usr/bin/env python3
"""Generate or independently verify GlifiStore wire-protocol v2 fixtures."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

VERSION = 2
REQUEST_HEADER_BYTES = 40
RESPONSE_HEADER_BYTES = 40
NO_WORKER = 0xFFFFFFFF


def write_hex(path: Path, payload: bytes) -> None:
    lines = []
    for offset in range(0, len(payload), 16):
        lines.append(" ".join(f"{byte:02x}" for byte in payload[offset : offset + 16]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_hex(path: Path) -> bytes:
    return bytes(int(token, 16) for token in path.read_text(encoding="utf-8").split())


def request(
    opcode: int,
    request_id: int,
    *,
    key: bytes = b"",
    value: bytes = b"",
    expire_at_ns: int = 0,
    target_worker: int = NO_WORKER,
) -> bytes:
    frame_size = REQUEST_HEADER_BYTES + len(key) + len(value)
    header = struct.pack(
        "<IHBBQIIQII",
        frame_size,
        VERSION,
        opcode,
        0,
        request_id,
        len(key),
        len(value),
        expire_at_ns,
        target_worker,
        0,
    )
    return header + key + value


def response(
    status: int,
    request_id: int,
    *,
    value: bytes = b"",
    owner_worker: int = NO_WORKER,
    worker_count: int = 4,
    routing_epoch: int = 9,
) -> bytes:
    frame_size = RESPONSE_HEADER_BYTES + len(value)
    return struct.pack(
        "<IHHQIIIIQ",
        frame_size,
        VERSION,
        status,
        request_id,
        len(value),
        owner_worker,
        worker_count,
        0,
        routing_epoch,
    ) + value


def request_corpus() -> bytes:
    frames = (
        request(1, 1),
        request(2, 2, value=b"\x00ping\xff"),
        request(3, 3, key=b"get\x00key"),
        request(4, 4, key=b"put\x00key", value=b"\x10\x20\xff", expire_at_ns=123456789),
        request(5, 5, key=b"erase-key"),
        request(6, 6, target_worker=2),
        request(7, 7),
        request(8, 8),
        request(9, 9),
        request(10, 10, key=b"/tmp/glifistore-backup"),
    )
    return b"".join(frames)


def response_corpus() -> bytes:
    return b"".join(
        response(status, 100 + status, value=b"GlifiStore/2" if status == 0 else b"",
                 owner_worker=2 if status == 6 else NO_WORKER)
        for status in range(9)
    )


def split_frames(data: bytes, header_bytes: int) -> list[bytes]:
    frames = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < header_bytes:
            raise ValueError("fixture ends inside a frame header")
        frame_size = struct.unpack_from("<I", data, offset)[0]
        if frame_size < header_bytes or frame_size > len(data) - offset:
            raise ValueError("fixture contains an invalid frame extent")
        frames.append(data[offset : offset + frame_size])
        offset += frame_size
    return frames


def verify_requests(data: bytes) -> None:
    frames = split_frames(data, REQUEST_HEADER_BYTES)
    if len(frames) != 10:
        raise ValueError("request corpus must contain every protocol-v2 opcode")
    for expected_opcode, frame in enumerate(frames, start=1):
        frame_size, version, opcode, flags, request_id, key_size, value_size = struct.unpack_from(
            "<IHBBQII", frame, 0
        )
        if (frame_size, version, opcode, flags, request_id) != (
            len(frame), VERSION, expected_opcode, 0, expected_opcode
        ):
            raise ValueError("request fixture header mismatch")
        if REQUEST_HEADER_BYTES + key_size + value_size != len(frame):
            raise ValueError("request fixture payload extent mismatch")
        if struct.unpack_from("<I", frame, 36)[0] != 0:
            raise ValueError("request fixture reserved field is nonzero")
        expire_at_ns, target_worker = struct.unpack_from("<QI", frame, 24)
        if expected_opcode == 6:
            if target_worker == NO_WORKER:
                raise ValueError("BIND_WORKER fixture must set target_worker")
        elif target_worker != NO_WORKER:
            raise ValueError("non-BIND fixture target_worker must be NO_WORKER")
        if expected_opcode in (1, 7, 8, 9) and (key_size or value_size or expire_at_ns):
            raise ValueError("lifecycle/INIT fixture must have empty payloads")
        if expected_opcode == 2 and (key_size or expire_at_ns):
            raise ValueError("PING fixture cannot carry key or expiry")
        if expected_opcode in (3, 5) and (value_size or expire_at_ns or key_size == 0):
            raise ValueError("GET/ERASE fixture must carry a key without value/expiry")
        if expected_opcode == 4 and key_size == 0:
            raise ValueError("PUT fixture requires a key")
        if expected_opcode == 6 and (key_size or value_size or expire_at_ns):
            raise ValueError("BIND_WORKER fixture cannot carry key, value, or expiry")
        if expected_opcode == 10 and (value_size or expire_at_ns or key_size == 0):
            raise ValueError("BACKUP fixture must carry a destination path key without value/expiry")


def verify_responses(data: bytes) -> None:
    frames = split_frames(data, RESPONSE_HEADER_BYTES)
    if len(frames) != 9:
        raise ValueError("response corpus must contain every protocol-v2 status")
    for expected_status, frame in enumerate(frames):
        frame_size, version, status, request_id, value_size = struct.unpack_from("<IHHQI", frame, 0)
        if (frame_size, version, status, request_id) != (
            len(frame), VERSION, expected_status, 100 + expected_status
        ):
            raise ValueError("response fixture header mismatch")
        if RESPONSE_HEADER_BYTES + value_size != len(frame):
            raise ValueError("response fixture payload extent mismatch")
        if struct.unpack_from("<I", frame, 28)[0] != 0:
            raise ValueError("response fixture reserved field is nonzero")


def generate(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    write_hex(directory / "wire_requests_v2.hex", request_corpus())
    write_hex(directory / "wire_responses_v2.hex", response_corpus())


def verify(directory: Path) -> None:
    verify_requests(read_hex(directory / "wire_requests_v2.hex"))
    verify_responses(read_hex(directory / "wire_responses_v2.hex"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        generate(args.output_dir)
        verify(args.output_dir)
        print(f"Generated and verified wire fixtures in {args.output_dir}")
    if args.verify:
        verify(args.verify)
        print(f"Verified wire fixtures in {args.verify}")
    if not args.output_dir and not args.verify:
        parser.error("Specify --output-dir and/or --verify")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
