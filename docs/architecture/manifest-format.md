# Manifest format

**Normative** · Manifest v1

The byte layout of Manifest v1, the authoritative durable catalog of one Store, and the rules that
select one Manifest among valid candidates.

Manifest v1 is an explicitly encoded little-endian byte sequence: one fixed 128-byte header followed
by fixed 32-byte Segment entries. The file has no implicit padding or trailing bytes, and its CRC32C
covers the complete encoded extent.

## Header v1

| Offset | Bytes | Field | v1 rule |
|---:|---:|---|---|
| 0 | 4 | manifest magic | `GLYM` (`47 4c 59 4d`) |
| 4 | 2 | manifest format version | `1` |
| 6 | 2 | header size | `128` |
| 8 | 4 | total encoded size | exactly `128 + segment_count * 32` |
| 12 | 2 | Segment entry size | `32` |
| 14 | 2 | flags | zero in v1 |
| 16 | 16 | Store ID | non-zero and equal to every listed Segment header |
| 32 | 8 | manifest generation | non-zero and monotonic across publications |
| 40 | 4 | routing algorithm | `1` = `fnv1a64-v1`; `2` = `siphash24-v1` (ADR 0030) |
| 44 | 4 | Worker count | 1–256 and fixed after Store creation |
| 48 | 8 | routing epoch | non-zero and fixed without an explicit migration |
| 56 | 4 | Segment count | at most 1,000,000 in v1 |
| 60 | 2 | Segment format version | `1` |
| 62 | 2 | Record format version | `1` |
| 64 | 2 | Segment header format version | `1` |
| 66 | 2 | commit-slot format version | `1` |
| 68 | 8 | next Segment ID | non-zero and greater than every catalog ID |
| 76 | 4 | next Segment generation | non-zero |
| 80 | 4 | manifest CRC32C | computed over the complete file with this field zero |
| 84 | 8 | Worker hash seed | `0` for FNV; SipHash seed for `siphash24-v1` |
| 92 | 36 | reserved | zero in v1 |

The one-million-entry limit bounds v1 decoding to 32,000,128 bytes. A decoder validates the count
and the checked encoded extent before it allocates the catalog or the temporary checksum buffer.
Unknown required versions fail closed even when the checksum is otherwise valid.

## Segment entry v1

Entry offsets are relative to the beginning of each 32-byte entry.

| Offset | Bytes | Field | v1 rule |
|---:|---:|---|---|
| 0 | 8 | Segment ID | non-zero, unique, strictly increasing across entries |
| 8 | 4 | Segment generation | non-zero |
| 12 | 4 | owner Worker | less than the persisted Worker count |
| 16 | 2 | manifest role | `1` active, `2` sealed |
| 18 | 2 | flags | zero in v1 |
| 20 | 12 | reserved | zero in v1 |

The catalog contains exactly one active Segment for every Worker. Sealed Segments stay listed while
they are recovery sources. Retiring or deleting one requires a later Manifest publication that
removes it, so v1 does not persist retired or reclaimable roles.

The Manifest records Segment ID and generation, not a trusted path identity. The storage layer maps
those values to its canonical namespace, then verifies Store ID, Segment ID, generation, owner and
encoded versions against the Segment header. A filename alone is never recovery authority.

## Canonical encoding and selection

Strict Segment-ID order makes the encoded catalog canonical and rejects duplicate identities without
hash-based allocation. The newest Manifest is selected only from candidates that have already passed
complete decoding and invariant validation, and selection takes the greatest manifest generation.
Two different valid candidates with the same generation are corruption; identical duplicates select
the first candidate deterministically.

The canonical fixture is
[`tests/fixtures/manifest_v1.hex`](../../tests/fixtures/manifest_v1.hex). Automated tests verify
exact encoding, full-file checksum coverage, truncation and trailing-byte rejection, allocation
bounds, catalog ownership, active-Worker coverage, unknown-version failure and publication
ambiguity.

## Outside this codec

Filesystem naming, locking, temporary-file cleanup, atomic replacement and directory
synchronization are outside this codec. The first implementation layer is the
[persistence filesystem layer](persistence-filesystem.md); its integration is governed by
[durability and recovery](durability-recovery.md). Supported component combinations are in the
[format compatibility matrix](format-compatibility.md).

Embedded durable recovery is implemented; released-artifact compatibility and complete native
power-loss evidence remain release gates.
