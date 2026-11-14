# Segment and Record format

**Normative** · Segment, Segment header, commit slot and Record v1

The v1 byte layouts of a Segment, its header and commit slots, and a Record. Embedded Store
recovery and all v1 durability modes are implemented, but the on-disk contract is not yet declared
stable: migration policy, released-artifact compatibility and complete native power-loss evidence
remain release gates.

## Segment

Every Segment is exactly 64 MiB. The first 4 KiB hold the explicitly encoded v1 header. Record bytes
begin at offset 4096 and are appended with 8-byte alignment. All integer fields are unsigned
little-endian values; persisted bytes are never interpreted through C++ object layout.

```text
0                    4096                                      64 MiB
+--------------------+----------------------------------------------+
| encoded header     | Record | Record | Record | unused bytes      |
+--------------------+----------------------------------------------+
```

Normal Records do not cross Segment boundaries and are limited to 1 MiB in the prototype. A future
large-object path must use explicit chunking rather than weaken this invariant.

Segment lifecycle:

```text
FREE -> ACTIVE -> SEALED -> RETIRED -> RECLAIMABLE -> FREE/deleted
```

The runtime catalog tracks residency, liveness, readers, dirty state, persistence and retirement
epoch separately. Volatile pointers and file descriptors never appear in the persistent header.

Recovery scans only through the committed end recorded by the newest valid commit slot. Bytes beyond
that boundary are an uncommitted crash tail; invalid bytes before it are corruption. Ordering and
failure behavior are in [durability and recovery](durability-recovery.md).

### Header v1

The 4 KiB header holds a 128-byte immutable prefix and two independently checksummed 128-byte
commit slots. Bytes 384 through 4095 are reserved and must be zero in v1.

| Offset | Bytes | Field | v1 rule |
|---:|---:|---|---|
| 0 | 4 | header magic | `GLYH` (`47 4c 59 48`) |
| 4 | 2 | header format version | `1` |
| 6 | 2 | immutable prefix size | `128` |
| 8 | 2 | Segment format version | `1` |
| 10 | 2 | Record format version | `1` |
| 12 | 4 | total header size | `4096` |
| 16 | 16 | Store ID | non-zero opaque identity |
| 32 | 8 | Segment ID | non-zero |
| 40 | 4 | generation | non-zero |
| 44 | 4 | owner Worker | zero-based Worker ID |
| 48 | 4 | flags | zero in v1 |
| 52 | 4 | immutable CRC32C | computed over bytes 0–127 with this field zero |
| 56 | 72 | reserved | zero in v1 |
| 128 | 128 | commit slot 0 | slot layout below |
| 256 | 128 | commit slot 1 | slot layout below |
| 384 | 3712 | reserved | zero in v1 |

The header and commit magic values follow the Record convention: the hexadecimal constants are
encoded little-endian, so their bytes stay readable in a hex dump. The immutable checksum covers
exactly the first 128 bytes and therefore excludes the commit slots.

### Commit slot v1

Slot offsets are relative to the beginning of either slot. An entirely zero slot is empty.

| Offset | Bytes | Field | v1 rule |
|---:|---:|---|---|
| 0 | 4 | commit magic | `GLYC` (`47 4c 59 43`) |
| 4 | 2 | slot format version | `1` |
| 6 | 2 | slot size | `128` |
| 8 | 8 | commit generation | non-zero and monotonic |
| 16 | 4 | committed end | 4096–67108864, 8-byte aligned |
| 20 | 2 | lifecycle state | `1` active, `2` sealed |
| 22 | 2 | flags | zero in v1 |
| 24 | 8 | committed Record count | zero only for an empty extent |
| 32 | 8 | first committed sequence | zero only for an empty extent |
| 40 | 8 | last committed sequence | zero only for an empty extent; otherwise at least first |
| 48 | 4 | slot CRC32C | computed over all 128 bytes with this field zero |
| 52 | 76 | reserved | zero in v1 |

Recovery checks each slot independently and selects the valid decoded slot with the greatest
commit generation. A torn or malformed slot may fall back to the other slot. A correctly checksummed
slot with an unknown version fails closed, because silently selecting an older commit could lose
data. Two different valid slots with the same generation are corruption; identical duplicates
select slot 0 deterministically.

The canonical first 384 bytes are in
[`tests/fixtures/segment_header_v1.hex`](../../tests/fixtures/segment_header_v1.hex). Automated
tests verify exact encoding, decoding, reserved zeros, fallback after slot corruption, version
rejection and ambiguous-generation failure.

## Record

Record v1 has a fixed 56-byte header followed by the key bytes, the value bytes and the minimum zero
padding needed for 8-byte alignment.

| Offset | Bytes | Field | v1 rule |
|---:|---:|---|---|
| 0 | 4 | Record magic | `GLYR` (`47 4c 59 52`) |
| 4 | 2 | Record format version | `1` |
| 6 | 2 | Record header size | `56` |
| 8 | 4 | total encoded size | 56–1048576, exactly the minimum 8-byte-aligned extent |
| 12 | 4 | key size | exact byte length |
| 16 | 4 | value size | exact byte length |
| 20 | 4 | Record CRC32C | computed over the complete Record with this field zero |
| 24 | 8 | sequence | Worker mutation sequence |
| 32 | 8 | key hash | deterministic full-key hash metadata |
| 40 | 8 | expiration | absolute Unix-epoch nanoseconds; zero means none |
| 48 | 2 | opcode | `1` put, `2` erase |
| 50 | 2 | value type | `1` bytes, `2` integer, `3` map, `4` lease |
| 52 | 4 | flags | opcode/type-specific flags |
| 56 | variable | key and value | exact key bytes followed by exact value bytes |
| final | 0–7 | alignment padding | zero; no over-allocation is canonical |

The checksum field is treated as zero while CRC32C is computed. Decoders validate every extent
before creating a view, and never interpret persisted bytes by casting them to a C++ struct. The
canonical binary-key fixture is
[`tests/fixtures/record_v1.hex`](../../tests/fixtures/record_v1.hex).

## Related contracts

The Manifest, Segment header, commit slot and Record codecs have exact v1 layouts and golden
fixtures. The authoritative catalog layout is the [Manifest format](manifest-format.md). Segment
creation, alternate-slot append and seal, and the committed-boundary scan are in
[durable Segment files](segment-filesystem.md). Supported reader and writer pairs are in the
[format compatibility matrix](format-compatibility.md).
