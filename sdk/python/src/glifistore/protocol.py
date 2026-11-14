"""Canonical wire-protocol v2 codec and Worker routing."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from struct import Struct, pack, unpack_from

VERSION = 2
REQUEST_HEADER_BYTES = 40
RESPONSE_HEADER_BYTES = 40
MAX_FRAME_BYTES = 2 * 1024 * 1024
NO_WORKER = 0xFFFF_FFFF
IDENTITY = b"GlifiStore/2"
ROUTING_ALG_FNV1A64_V1 = 1
ROUTING_ALG_SIPHASH24_V1 = 2
WORKER_ROUTING_SIP_KEY1_XOR = 0x6A09_E667_F3BC_C909
INIT_IDENTITY_EXTENDED_BYTES = len(IDENTITY) + 1 + 4 + 8

_REQUEST_HEADER = Struct("<IHBBQIIQII")
_RESPONSE_HEADER = Struct("<IHHQIIIIQ")
_U64_MASK = 0xFFFF_FFFF_FFFF_FFFF


class Opcode(IntEnum):
    INIT = 1
    PING = 2
    GET = 3
    PUT = 4
    ERASE = 5
    BIND_WORKER = 6
    HEALTH = 7
    READY = 8
    STATS = 9
    BACKUP = 10


class Status(IntEnum):
    OK = 0
    INVALID_REQUEST = 1
    UNSUPPORTED = 2
    INTERNAL_ERROR = 3
    NOT_FOUND = 4
    OVERLOADED = 5
    WRONG_OWNER = 6
    NOT_BOUND = 7
    PERMISSION_DENIED = 8


@dataclass(frozen=True, slots=True)
class Request:
    opcode: Opcode
    request_id: int
    expire_at_ns: int = 0
    target_worker: int = NO_WORKER
    key: bytes = b""
    value: bytes = b""


@dataclass(frozen=True, slots=True)
class Response:
    status: Status
    request_id: int
    owner_worker: int
    worker_count: int
    routing_epoch: int
    value: bytes


def _u64(value: int, field: str) -> int:
    if not 0 <= value <= 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError(f"{field} is outside unsigned 64-bit range")
    return value


def _u32(value: int, field: str) -> int:
    if not 0 <= value <= 0xFFFF_FFFF:
        raise ValueError(f"{field} is outside unsigned 32-bit range")
    return value


def _validate_request_fields(
    opcode: Opcode,
    key: bytes,
    value: bytes,
    expire_at_ns: int,
    target_worker: int,
) -> None:
    if opcode is Opcode.INIT and (key or value or expire_at_ns != 0 or target_worker != NO_WORKER):
        raise ValueError("INIT request cannot carry key, value, expiry, or target_worker")
    if opcode is Opcode.PING and (key or expire_at_ns != 0 or target_worker != NO_WORKER):
        raise ValueError("PING request cannot carry key, expiry, or target_worker")
    if opcode is Opcode.GET and (
        not key or value or expire_at_ns != 0 or target_worker != NO_WORKER
    ):
        raise ValueError("GET request requires a key and cannot carry value, expiry, or target_worker")
    if opcode is Opcode.PUT and (not key or target_worker != NO_WORKER):
        raise ValueError("PUT request requires a key and cannot carry target_worker")
    if opcode is Opcode.ERASE and (
        not key or value or expire_at_ns != 0 or target_worker != NO_WORKER
    ):
        raise ValueError("ERASE request requires a key and cannot carry value, expiry, or target_worker")
    if opcode is Opcode.BIND_WORKER and (key or value or expire_at_ns != 0):
        raise ValueError("BIND_WORKER request cannot carry key, value, or expiry")
    if opcode is Opcode.BIND_WORKER and target_worker == NO_WORKER:
        raise ValueError("BIND_WORKER request requires an explicit target_worker")
    if opcode in (Opcode.HEALTH, Opcode.READY, Opcode.STATS) and (
        key or value or expire_at_ns != 0 or target_worker != NO_WORKER
    ):
        raise ValueError("lifecycle probe cannot carry key, value, expiry, or target_worker")
    if opcode is Opcode.BACKUP and (
        not key or value or expire_at_ns != 0 or target_worker != NO_WORKER
    ):
        raise ValueError(
            "BACKUP requires a destination path key and no value, expiry, or target_worker"
        )


def encode_request(
    opcode: Opcode,
    request_id: int,
    *,
    key: bytes = b"",
    value: bytes = b"",
    expire_at_ns: int = 0,
    target_worker: int = NO_WORKER,
) -> bytes:
    """Encode one canonical protocol-v2 request frame."""
    frame_size, key, value = _validated_request_size(
        opcode,
        request_id,
        key=key,
        value=value,
        expire_at_ns=expire_at_ns,
        target_worker=target_worker,
    )
    return _encode_validated_request_header(
        frame_size,
        opcode,
        request_id,
        key_size=len(key),
        value_size=len(value),
        expire_at_ns=expire_at_ns,
        target_worker=target_worker,
    ) + key + value


def _encode_validated_request_header(
    frame_size: int,
    opcode: Opcode,
    request_id: int,
    *,
    key_size: int,
    value_size: int,
    expire_at_ns: int = 0,
    target_worker: int = NO_WORKER,
) -> bytes:
    """Encode the header of a request whose fields and frame size were validated."""
    return _REQUEST_HEADER.pack(
        frame_size,
        VERSION,
        opcode,
        0,
        request_id,
        key_size,
        value_size,
        expire_at_ns,
        target_worker,
        0,
    )


def _validated_request_size(
    opcode: Opcode,
    request_id: int,
    *,
    key: bytes | bytearray | memoryview = b"",
    value: bytes | bytearray | memoryview = b"",
    expire_at_ns: int = 0,
    target_worker: int = NO_WORKER,
) -> tuple[int, bytes, bytes]:
    """Validate one request and return its size plus immutable payload views."""
    if not isinstance(opcode, Opcode):
        raise ValueError("opcode is not defined by wire protocol v2")
    if not isinstance(key, (bytes, bytearray, memoryview)) or not isinstance(
        value, (bytes, bytearray, memoryview)
    ):
        raise TypeError("key and value must be bytes-like")
    key = bytes(key)
    value = bytes(value)
    _validate_request_fields(opcode, key, value, expire_at_ns, target_worker)
    _u64(request_id, "request_id")
    _u64(expire_at_ns, "expire_at_ns")
    _u32(target_worker, "target_worker")
    frame_size = REQUEST_HEADER_BYTES + len(key) + len(value)
    if frame_size > MAX_FRAME_BYTES:
        raise ValueError("request exceeds the protocol frame limit")
    return frame_size, key, value


def decode_request(
    frame: bytes | bytearray | memoryview,
    maximum_frame_bytes: int = MAX_FRAME_BYTES,
) -> Request:
    """Decode one complete request and reject noncanonical fields."""
    if len(frame) < REQUEST_HEADER_BYTES:
        raise ValueError("request is shorter than its header")
    (
        frame_size,
        version,
        opcode,
        flags,
        request_id,
        key_size,
        value_size,
        expire_at_ns,
        target_worker,
        reserved,
    ) = _REQUEST_HEADER.unpack_from(frame)
    if frame_size != len(frame) or frame_size > maximum_frame_bytes:
        raise ValueError("request frame extent is invalid")
    if version != VERSION:
        raise ValueError("request protocol version is unsupported")
    if flags != 0 or reserved != 0:
        raise ValueError("request canonical fields are invalid")
    if REQUEST_HEADER_BYTES + key_size + value_size != frame_size:
        raise ValueError("request payload extent is invalid")
    try:
        decoded_opcode = Opcode(opcode)
    except ValueError as error:
        raise ValueError("request opcode is unknown") from error
    key_start = REQUEST_HEADER_BYTES
    value_start = key_start + key_size
    key = bytes(frame[key_start:value_start])
    value = bytes(frame[value_start : value_start + value_size])
    _validate_request_fields(decoded_opcode, key, value, expire_at_ns, target_worker)
    return Request(
        opcode=decoded_opcode,
        request_id=request_id,
        expire_at_ns=expire_at_ns,
        target_worker=target_worker,
        key=key,
        value=value,
    )


def encode_response(
    status: Status,
    request_id: int,
    *,
    value: bytes = b"",
    owner_worker: int = NO_WORKER,
    worker_count: int = 0,
    routing_epoch: int = 0,
) -> bytes:
    """Encode one canonical protocol-v2 response frame."""
    if not isinstance(status, Status):
        raise ValueError("status is not defined by wire protocol v2")
    _u64(request_id, "request_id")
    _u32(owner_worker, "owner_worker")
    _u32(worker_count, "worker_count")
    _u64(routing_epoch, "routing_epoch")
    frame_size = RESPONSE_HEADER_BYTES + len(value)
    if frame_size > MAX_FRAME_BYTES:
        raise ValueError("response exceeds the protocol frame limit")
    return _RESPONSE_HEADER.pack(
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


def decode_response(
    frame: bytes | bytearray | memoryview,
    maximum_frame_bytes: int = MAX_FRAME_BYTES,
) -> Response:
    """Decode one complete response and reject noncanonical fields."""
    return _decode_response_from(frame, 0, len(frame), maximum_frame_bytes)


def _decode_response_from(
    buffer: bytes | bytearray | memoryview,
    offset: int,
    extent: int,
    maximum_frame_bytes: int = MAX_FRAME_BYTES,
) -> Response:
    """Decode one complete response at an offset without creating a frame view."""
    if extent < RESPONSE_HEADER_BYTES:
        raise ValueError("response is shorter than its header")
    if offset < 0 or extent > len(buffer) - offset:
        raise ValueError("response frame extent is invalid")
    (
        frame_size,
        version,
        status,
        request_id,
        value_size,
        owner_worker,
        worker_count,
        reserved,
        routing_epoch,
    ) = _RESPONSE_HEADER.unpack_from(buffer, offset)
    if frame_size != extent or frame_size > maximum_frame_bytes:
        raise ValueError("response frame extent is invalid")
    if version != VERSION:
        raise ValueError("response protocol version is unsupported")
    if reserved != 0:
        raise ValueError("response reserved field is noncanonical")
    if RESPONSE_HEADER_BYTES + value_size != frame_size:
        raise ValueError("response value extent is invalid")
    try:
        decoded_status = Status(status)
    except ValueError as error:
        raise ValueError("response status is unknown") from error
    return Response(
        status=decoded_status,
        request_id=request_id,
        owner_worker=owner_worker,
        worker_count=worker_count,
        routing_epoch=routing_epoch,
        value=bytes(buffer[offset + RESPONSE_HEADER_BYTES : offset + frame_size]),
    )


@dataclass(frozen=True, slots=True)
class WorkerRouting:
    """Process / session Worker ownership algorithm (ADR 0030)."""

    algorithm: int = ROUTING_ALG_FNV1A64_V1
    seed: int = 0

    @property
    def keyed(self) -> bool:
        return self.algorithm == ROUTING_ALG_SIPHASH24_V1


def fnv1a64(key: bytes) -> int:
    value = 14_695_981_039_346_656_037
    for byte in key:
        value ^= byte
        value = (value * 1_099_511_628_211) & _U64_MASK
    return value


def siphash24(key: bytes, k0: int, k1: int) -> int:
    """SipHash-2-4 matching the C++ core (Aumasson/Bernstein)."""
    k0 &= _U64_MASK
    k1 &= _U64_MASK

    def rotl(value: int, shift: int) -> int:
        return ((value << shift) | (value >> (64 - shift))) & _U64_MASK

    def sipround(v0: int, v1: int, v2: int, v3: int) -> tuple[int, int, int, int]:
        v0 = (v0 + v1) & _U64_MASK
        v1 = rotl(v1, 13)
        v1 ^= v0
        v0 = rotl(v0, 32)
        v2 = (v2 + v3) & _U64_MASK
        v3 = rotl(v3, 16)
        v3 ^= v2
        v0 = (v0 + v3) & _U64_MASK
        v3 = rotl(v3, 21)
        v3 ^= v0
        v2 = (v2 + v1) & _U64_MASK
        v1 = rotl(v1, 17)
        v1 ^= v2
        v2 = rotl(v2, 32)
        return v0, v1, v2, v3

    v0 = k0 ^ 0x736F_6D65_7073_6575
    v1 = k1 ^ 0x646F_7261_6E64_6F6D
    v2 = k0 ^ 0x6C79_6765_6E65_7261
    v3 = k1 ^ 0x7465_6462_7974_6573
    length = len(key)
    offset = 0
    while offset + 8 <= length:
        message = int.from_bytes(key[offset : offset + 8], "little")
        v3 ^= message
        v0, v1, v2, v3 = sipround(v0, v1, v2, v3)
        v0, v1, v2, v3 = sipround(v0, v1, v2, v3)
        v0 ^= message
        offset += 8
    message = length << 56
    for index, byte in enumerate(key[offset:]):
        message |= byte << (8 * index)
    v3 ^= message
    v0, v1, v2, v3 = sipround(v0, v1, v2, v3)
    v0, v1, v2, v3 = sipround(v0, v1, v2, v3)
    v0 ^= message
    v2 ^= 0xFF
    for _ in range(4):
        v0, v1, v2, v3 = sipround(v0, v1, v2, v3)
    return v0 ^ v1 ^ v2 ^ v3


def validate_worker_routing(routing: WorkerRouting) -> None:
    if routing.algorithm == ROUTING_ALG_FNV1A64_V1:
        if routing.seed != 0:
            raise ValueError("fnv1a64-v1 Worker routing requires a zero hash seed")
        return
    if routing.algorithm == ROUTING_ALG_SIPHASH24_V1:
        return
    raise ValueError("unsupported Worker routing algorithm")


def hash_key_routing(key: bytes, routing: WorkerRouting | None = None) -> int:
    state = routing or WorkerRouting()
    validate_worker_routing(state)
    if state.algorithm == ROUTING_ALG_SIPHASH24_V1:
        return siphash24(key, state.seed, state.seed ^ WORKER_ROUTING_SIP_KEY1_XOR)
    return fnv1a64(key)


def encode_init_identity(routing: WorkerRouting | None = None) -> bytes:
    state = routing or WorkerRouting()
    validate_worker_routing(state)
    if not state.keyed:
        return IDENTITY
    return IDENTITY + b"\x00" + pack("<IQ", state.algorithm, state.seed)


def decode_init_identity(value: bytes) -> WorkerRouting:
    """Parse INIT identity: plain FNV or extended SipHash (fail closed)."""
    if value == IDENTITY:
        return WorkerRouting()
    if len(value) != INIT_IDENTITY_EXTENDED_BYTES:
        raise ValueError("server INIT identity value has unexpected length")
    if value[: len(IDENTITY)] != IDENTITY or value[len(IDENTITY)] != 0:
        raise ValueError("server INIT identity prefix is invalid")
    algorithm, seed = unpack_from("<IQ", value, len(IDENTITY) + 1)
    state = WorkerRouting(algorithm=algorithm, seed=seed)
    validate_worker_routing(state)
    if not state.keyed:
        raise ValueError("server INIT extended identity must use siphash24-v1 routing")
    return state


def worker_for(key: bytes, worker_count: int, routing: WorkerRouting | None = None) -> int:
    if worker_count <= 0:
        raise ValueError("worker_count must be positive")
    return hash_key_routing(key, routing) % worker_count
