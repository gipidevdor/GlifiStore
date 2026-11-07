Status: normative for the opt-in embedded prototype; release qualification remains open
Applies to: Blob File v1 / BlobRef v1 in opt-in persistence family v2; embedded milestone first
Owner: storage, concurrency, API and release maintainers
Last reviewed: 2026-11-07

# Large Blob Storage v1

[ADR 0042](../adr/0042-large-blob-storage.md) was accepted for implementation on 2026-11-07.
The embedded prototype implements the opt-in v2 profile, streaming and owning reads/writes,
recovery, conservative GC and backup. Enable it with `StoreConfig::enable_blobs = true` on a new
**durable paired embedded Store**. Existing [persistence v1](persistence-v1.md) Stores are not upgraded.
Daemon/SDK blob transfer and E3/E4 certification remain unavailable; the project is still an
architectural prototype. Local tests are not release acceptance or a cross-platform certification.

Implementation: [public handles](../../include/glifistore/store/blob.hpp),
[Store integration](../../src/store/store_blob.cpp),
[blob storage](../../src/persistence/blob_storage.cpp). Retained results and remaining qualification
work belong to the [blob gate](../../engineering/gates/large-blobs.yaml).

## 1. Limits, placement and logical semantics

| Quantity | Proposed contract |
| --- | --- |
| Maximum logical value | 67,108,864 bytes (64 MiB), excluding key and all metadata |
| Normal encoded Record | At most 1,048,576 bytes, minimum 8-byte aligned extent |
| Segment | Exactly 67,108,864 bytes; Records never cross its boundary |
| Blob chunk | 1,048,576 bytes, except a shorter final chunk |
| Chunks per nonempty blob | `1 + (logical_size - 1) / 1,048,576`, at most 64 |
| BlobRef encoded size | 80 bytes |
| Key in a blob-capable Store | Nonempty, at most 1,048,440 bytes (`1 MiB - 56 - 80`) |
| Empty value | Valid inline value; not a tombstone or empty blob file |

The v2 key bound applies to every PUT in a blob-capable Store, including a small inline value.
It guarantees that **every accepted key** can also hold a maximum-size blob. Legacy v1 key
admission is unchanged. All extent calculations, including alignment and `off_t` conversion,
are checked before allocation or I/O.

For an accepted key, use inline bytes exactly when
`align_up(56 + key_size + logical_value_size, 8) <= 1,048,576`; otherwise use BlobRef. Thus the
inline value ceiling depends on key length and is strictly below 1 MiB. A 1 MiB value is a blob.
The 64 MiB maximum value remains independent of accepted key length.

PUT replaces the previous value regardless of placement. GET returns logical application bytes,
never BlobRef bytes. ERASE uses the existing logical tombstone rule, and TTL uses the Store clock.
Opening a streaming read selects one visible version and evaluates TTL once; later overwrite,
erase or expiration does not splice or truncate that stream. There is no multi-key transaction,
CAS, content deduplication, online rebalance, or compression in this version.

## 2. Explicit compatibility boundary

The engine supports two separate profiles. Creation of a blob-capable Store is explicit;
opening a v1 Store never upgrades it or changes its limits. New-format constants must not replace
the legacy codec constants globally.

| Component | Legacy profile | Proposed blob profile |
| --- | --- | --- |
| Persistence family | v1 | v2 |
| Manifest | v1, 128-byte header | v2, 128-byte header with fields below |
| Segment physical geometry | v1, 64 MiB | v1 geometry, still 64 MiB |
| Segment header | v1, required Record v1 | v2, required Record v2 |
| Commit slots | v1 | v1, same ordering and extents |
| Records | v1, 56-byte header | v2, same header geometry and 1 MiB bound |
| Bootstrap intent | Manifest v1 | Manifest v2 |
| Compaction intent | v1, embedded v1 Manifests | v2, embedded v2 Manifests |
| Blob files / references | Not recognized | Blob File v1 / BlobRef v1 |

Manifest v2 retains the v1 field offsets except: version at offset 4 is `2`, required Record
version at 62 is `2`, required Segment-header version at 64 is `2`, and bytes 92–127 become:

| Offset | Bytes | Field | Rule |
| ---: | ---: | --- | --- |
| 92 | 16 | next blob ID | Nonzero unsigned 128-bit high-water mark, little-endian |
| 108 | 8 | required feature bits | Exactly bit 0 (`external_blobs_v1`); unknown bits reject |
| 116 | 2 | Blob File version | `1` |
| 118 | 2 | BlobRef version | `1` |
| 120 | 2 | digest algorithm | `1` = SHA-256 |
| 122 | 2 | chunk size log2 | `20` |
| 124 | 4 | reserved | Zero |

The existing whole-Manifest CRC32C and checked `128 + segment_count * 32` extent still apply.
Segment-header v2 changes its header-version field at offset 4 to `2` and required Record version
at 10 to `2`; its other fields, CRC extent and slot offsets retain their existing definitions.
Every Record in a v2 Segment has Record version `2`, including inline PUT and ERASE. Record v2
allocates value type `5` to `blob_ref`; type `1` retains bytes semantics, flags remain zero, and
types 2–4 retain their allocated meanings without gaining public Store semantics. BlobRef is valid
only for PUT. Its Record value-size field is exactly `80`; logical size comes from the descriptor.

Compaction-intent v2 retains the v1 outer geometry, uses version `2`, and embeds only matching v2
Manifests. An old engine must reject an empty v2 Store and checksum-valid future versions without
falling back to an older Manifest, adopting orphan files, or modifying the directory. Unknown
required versions are incompatibility, not an excuse to salvage around them.

## 3. Namespace, identity and codecs

The v2 namespace adds a single engine-owned `blobs/` directory under the Store root. It contains
only `blob-<id>.glifi` and `.blob-<id>.tmp`, where `<id>` is exactly 32 lowercase hexadecimal digits
encoding the 16 Blob ID bytes in order. Creation synchronizes the parent directory before use.
Staging and final files reside in this same directory/filesystem. No user key is a pathname.
Descriptor-relative operations reject symlinks, non-regular files, identity mismatches, unexpected
hard links and unknown names. Files are private to the Store's exclusive directory lock.

Blob IDs are monotonic Store-scoped unsigned 128-bit identities, not content hashes or secrets.
A catalog allocator reserves a bounded ID range by synchronizing a new Manifest high-water mark
**before** exposing IDs from that range. After restart, unused reserved IDs are skipped. Concurrent
Manifest transitions serialize and preserve the greatest high-water mark. Allocation never wraps,
decreases or reuses an ID. Range reservation is outside the inline timed path. Initial range size
is one ID in this prototype; gaps do not carry logical mutations. Every
referenced ID must be below the selected Manifest's high-water mark.

Files use exclusive creation and installation that cannot replace an existing final identity.
The implementation must prove its atomic installation primitive on each supported native platform.
An existing final filename is an identity conflict, not a request to overwrite it. The portable implementation
uses exclusive staging creation and `linkat` without replacement, followed by staging-name unlink
and directory sync. A crash between link and unlink may leave exactly two private names for the
same inode; recovery recognizes only that exact pair as an uncommitted orphan. A referenced final
blob must have one link. It never admits the descriptor before the pair is reduced and synced. Orphan files
never become recovery authority merely because they contain a valid header.

All tables below describe explicit little-endian bytes. No C++ struct layout is persistent.
CRC32C parameters are exactly those in persistence v1. SHA-256 is an unkeyed integrity digest of
the exact logical value bytes in chunk order; it supplies neither authentication nor encryption.
The provider must work when optional TLS is disabled. Digest bytes are stored in normal SHA-256
output order, without integer/endian conversion.

### BlobRef v1: 80 bytes

| Offset | Bytes | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `GLBR` |
| 4 | 2 | descriptor version | `1` |
| 6 | 2 | encoded size | `80` |
| 8 | 16 | blob ID | Nonzero; bound to this Store and owner by the file header |
| 24 | 8 | logical size | `1..67,108,864` |
| 32 | 4 | chunk size | `1,048,576` |
| 36 | 4 | chunk count | Exact ceiling division, `1..64` |
| 40 | 2 | digest algorithm | `1` |
| 42 | 2 | flags | Zero |
| 44 | 4 | reserved | Zero |
| 48 | 32 | digest | SHA-256 of the complete logical value |

The enclosing Record CRC covers the entire descriptor, key and padding. A descriptor is accepted
only after its Record type/version, fields, file identity, sizes and digest agree. Full-key
equality and existing Worker routing remain authoritative; blob IDs do not select a new owner.

### Blob File v1 header: 128 bytes

| Offset | Bytes | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `GLYB` |
| 4 | 2 | file version | `1` |
| 6 | 2 | header size | `128` |
| 8 | 16 | blob ID | Equal to name and BlobRef |
| 24 | 16 | Store ID | Equal to accepted Manifest |
| 40 | 4 | owner Worker | Equal to the referring Record's owner |
| 44 | 4 | chunk size | `1,048,576` |
| 48 | 8 | logical size | Equal to BlobRef |
| 56 | 4 | chunk count | Equal to BlobRef |
| 60 | 2 | chunk entry size | `16` |
| 62 | 2 | digest algorithm | `1` |
| 64 | 8 | table offset | `128` |
| 72 | 8 | payload offset | `128 + 16 * chunk_count` |
| 80 | 8 | footer offset | `payload_offset + logical_size` |
| 88 | 8 | exact file size | `footer_offset + 64` |
| 96 | 4 | metadata CRC32C | Header plus complete table, this field zero |
| 100 | 4 | flags | Zero |
| 104 | 24 | reserved | Zero |

Each 16-byte chunk entry contains absolute payload offset (`u64`), length (`u32`) and CRC32C
(`u32`). Entries are ordered, contiguous and non-overlapping; each non-final length is exactly
1 MiB, the final length is `1..1 MiB`, and their checked sum equals logical size. Chunk CRC covers
exactly its payload bytes. Descriptor gaps, duplicate ranges and trailing bytes are not canonical
encodings. The format describes logical bytes, not a portable proof of physical allocation;
space reservation and physical-byte accounting remain separate controls. The largest complete
file is 67,110,080 bytes, not a 64 MiB Segment.

### Footer: 64 bytes

| Offset | Bytes | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `GLBF` |
| 4 | 2 | footer version | `1` |
| 6 | 2 | footer size | `64` |
| 8 | 8 | logical size | Equal to header and BlobRef |
| 16 | 32 | digest | Equal to BlobRef and recomputed SHA-256 |
| 48 | 4 | footer CRC32C | Entire footer, this field zero |
| 52 | 12 | reserved | Zero |

Decode fixed headers first; reject count/size/version inconsistencies before allocating a table
or scheduling payload I/O. A truncated file or failed integrity check is never returned as a
short successful value.

## 4. Ownership, scheduling and commit

Large payload I/O and hashing execute in a bounded pool outside the Reader and Writer. Existing
small descriptor persistence may still wait at the normal Writer commit boundary; this proposal
does not claim to eliminate the existing strict-mode synchronization cost.

```text
NEW -> RESERVED -> STAGING -> SEALED -> INSTALLED -> LOGICALLY_ADMITTED -> VISIBLE -> TERMINAL
           |          |          |          |
           +----------+----------+----------+-> ABORTED -> orphan cleanup
```

1. **Reserve:** validate key/value bounds and checked worst-case file size; acquire memory,
   transfer, file-count, metadata and physical-space credits. Obtain a never-reused blob ID only
   when the stream actually requires external placement. Reserve space against staging and
   installed/retired bytes. Charge the maximum of the staging extent (including its reserved
   chunk-table prefix) and the final encoded extent before file creation; free-space checks
   do not replace handling later ENOSPC/EDQUOT.
2. **Stage:** own the key/expiration and copy bounded input into a private file. Compute per-chunk
   CRC and rolling digest. An unknown-size stream can buffer the inline prefix; crossing the
   encoded inline limit spills to staging. Empty or inline-fitting completed streams use the
   ordinary inline Record path and produce no final blob file.
3. **Seal:** require exact expected size when specified; complete header/table/footer, independently
   reread and verify the file using bounded buffers, then perform the strict platform file-sync
   boundary. Errors leave the logical key unchanged.
4. **Install:** atomically publish the complete immutable file without replacement, then perform
   the platform directory-sync boundary. A failure or uncertain install prevents descriptor
   admission; retain/quarantine for recovery rather than treating it as success.
5. **Commit:** enqueue the small descriptor with its reservation/pin on the owner mutation lane.
   The Writer assigns its ordinary mutation sequence and uses the existing logical commit and
   immutable-generation publication protocol. The installed payload is already durable. No
   Reader can observe a prepared upload before logical publication. Queue ownership transfers
   the key and blob root before releasing the caller; no task depends on a raw upload handle.
6. **Finish:** release transfer buffers; transfer ownership of the blob root to committed history
   and published generations. Retire previous values through normal epoch/lease accounting.

The order is Writer **admission/serialization after preparation**, not begin-upload order. A slow
upload cannot hold a key or shard while a healthy inline mutation runs. Concurrent prepared
uploads to the same key obey their assigned Writer sequence; the last logical mutation wins.
After logical admission, abort/disconnect/timeout cannot cancel Store work. Pre-admission abort
is terminal and schedules bounded cleanup. Repeated commit on the same handle returns its cached
terminal outcome and cannot enqueue the mutation twice.
Terminal handles release copied keys and payload buffer allocations before returning transfer
credits, even when the caller retains the handle. Cleanup removes only staging names created
by that transfer; an unsuccessful exclusive create never grants ownership of an existing name.
If cleanup itself fails, the orphan remains charged until conservative reclamation succeeds.

### Durability policies

| Policy | Blob dependency before descriptor admission | Successful mutation ACK |
| --- | --- | --- |
| `durable_sync` | Verified file sync + installed directory sync | Existing strict descriptor commit, then required Reader visibility |
| `durable_group` | Same, for every group member | Existing strict group boundary and visibility; no early member ACK |
| `durable_periodic` | Same strict blob dependency | In-memory logical publication; descriptor persistence follows the periodic contract |
| `volatile_memory` | Unsupported for the initial blob milestone | Explicit unsupported-mode error; no implicit persistent side store |

`flush()` and successful `close()` cover every logically admitted blob descriptor under their
existing admission fence. They do not commit unfinished uploads. A periodic ACK can be lost
before a completed descriptor flush; a recovered descriptor must still refer to a complete blob.
The configured periodic interval is a scheduling target, not a new hard wall-clock guarantee
under stalled storage. Any stronger bound needs separate proof.

Once the logical commit may have happened, errors retain the existing committed/rejected/
indeterminate distinction. A post-commit publication error triggers the existing fail-closed
policy. Neither file installation nor an upload ID is evidence that the key mutation committed.

## 5. Recovery and reclamation

| Durable boundary observed after crash | Logical recovery | Blob treatment |
| --- | --- | --- |
| Partial staging, no descriptor | Previous key state | Recognized unreferenced temporary; bounded cleanup |
| Sealed/installed payload, no committed descriptor | Previous key state | Installed orphan; never adopt as a value |
| Descriptor beyond selected commit extent | Existing commit-slot rules | Treat only as uncommitted tail; retain if another root exists |
| Valid committed descriptor | Replay normal key/sequence/TTL semantics | Require matching complete verified payload |
| Committed descriptor with missing/bad payload | Reject open / fail closed | No fallback to the previous value or another file |
| Overwrite/erase committed, old descriptor still in catalog history | Latest logical mutation wins | Retain old payload until its recovery and Reader roots retire |

Recovery starts from accepted Manifest/intent authority and the exact selected committed extents.
It validates **every** referenced blob identity in that retained history, including descriptors
later overwritten or expired. Each distinct file is fully CRC/digest verified before opening the
Store for service, with bounded I/O concurrency, configured count/byte limits and observable work.
Unknown namespaces, contradictory identities or a verification budget failure never permit a
partially recovered Store. Repeated references are accounted without repeated full-file hashing.

GC roots are: descriptors in all still-recoverable committed Segment extents; all selectable
Manifest/compaction-intent candidates still needed by recovery; admitted or prepared publications;
all descriptor records in pending append/group/periodic-flush extents until rollback or transfer
to committed-history roots; current/retired generations and active stream/cold-I/O leases; and
backup/migration snapshot pins. Periodic ACK completion does not release a dirty descriptor's
root, even if another mutation already replaced it in the Index. Logical reachability from the
latest Index alone is insufficient.

The implementation maintains a bounded derived root registry and verifies the committed closure. That state
is rebuildable and is not a second persistent authority. It must count memory and work against
configured metadata/catalog limits; overflow or incomplete marking disables deletion and applies
admission backpressure. Losing volatile GC progress on crash means retain and rebuild.

Before deleting any candidate, serialize its eligibility with publication, reservation, Reader
lease acquisition and catalog transitions. Atomically mark it deleting only after revalidating
zero roots; no new publication/lease may acquire that identity afterward. Unlink and directory
sync run outside the Reader/Writer on bounded I/O workers. Failed deletion retains its space
charge and retry state. Names are never reused, and post-crash orphan cleanup repeats the same
reachability check. No deletion is authorized by expiration, a negative cache, mtime or age alone.

Compaction copies typed descriptors and pins their payloads; it does not rewrite the payload.
Old roots retire only after durable Manifest replacement and the corresponding Reader quiescence.
V2 maintenance must allow budgeted descriptor-history compaction when it can free blob bytes even
if the number of Segment files would not decrease. It must retain tombstone suppression across
all retained history and obey normal copy/temporary-space budgets. V2 includes the unique obsolete
blob file bytes in the write-amplification denominator; equal Segment counts are never a budget
bypass. The prototype rotates before a blob mutation when the active log already has 32 Records.
Blob debt may require early
seal/rotation of an active Segment whose obsolete descriptors would otherwise pin files until
the 64 MiB log fills. Reserve bounded maintenance space/metadata credits separately so exhausted
upload admission cannot prevent the rotation/compaction needed to make progress. This is an
explicit v2 maintenance extension; the v1 no-gain policy is unchanged.

## 6. Streaming API and resource contract

The installed C++23 interface is declared in `glifistore/store/blob.hpp` and `Store`:

```cpp
struct BlobOptions {
    std::uint64_t expire_at_ns{};
    std::optional<std::uint64_t> expected_size; // nullopt unknown; 0 exactly empty
};
Result<std::unique_ptr<BlobWriter>> begin_blob_put(std::string_view key, BlobOptions);
Result<std::unique_ptr<BlobReader>> open_blob(std::string_view key);
// BlobWriter: Status write(span<const byte>), Status commit(), void abort() noexcept.
// BlobReader: Result<size_t> read(span<byte>), uint64_t size() const noexcept.
```

`write()` accepts at most 1 MiB per call, owns/copies accepted bytes before return, and performs
no work proportional to the complete value on a Reader/Writer. Repeated writes may cross physical
chunk boundaries. A per-handle mutex or explicit single-caller rule serializes operations; the
first implementation uses a single caller per handle. Destruction before logical admission
aborts; destruction afterward cannot undo the mutation. No owning raw `new`/`delete` is required.
A failed write makes the upload terminal and aborts its pre-admission state; a caller cannot
blindly resend a chunk after an error that may have written a prefix. Writes after terminal
commit/abort fail without side effects. Queue copies, if any, are charged against the same
transfer cap; buffer release or ownership moves must prevent a large key from exceeding it.

`commit()` seals and synchronously waits on the **calling thread**, while pool/Writer tasks make
progress independently. It is terminal for success, rejection or indeterminate outcome. Another
call on that handle returns the cached result. Handle-local idempotence does not survive process
restart; reconnect resolution is deferred to the network contract. No automatic blind retry is
promised. `expected_size` is optional rather than using zero for both unknown and empty.

`open_blob()` is a streaming view of either placement. A blob handle pins its exact version and
open file before releasing the generation lease. Each complete chunk is CRC-checked before any
of its bytes are delivered. Reads may return fewer bytes than requested; zero means validated
EOF, and the rolling digest is checked before the final successful delivery/EOF. On failure,
earlier delivered bytes cannot be retracted; callers requiring an owning value use GET, which
validates the whole value before returning it. Integrity failure is sticky and fails closed for
affected Store operations. A digest is not an adversarial authenticity guarantee.

Stream handles are Store-scoped: `close()` prevents new transfers, aborts unfinished uploads,
drains admitted commits and in-flight I/O, then revokes further handle reads/writes and releases
their pins before the directory lock. Surviving handle objects stay memory-safe and return
`unavailable`; no task retains a raw Store pointer after shutdown. Owning GET results retain their
existing lifetime independent of Store destruction. Cancellation generations prevent a late I/O
completion from touching a reused handle or releasing another transfer's credits.

Initial prototype defaults and hard transfer bounds (performance qualification remains open):

| Resource | Limit / accounting |
| --- | --- |
| Engine-owned streaming memory | At most 4 MiB per admitted transfer, including copied key, buffers, queued payload and integrity state |
| Payload in flight | At most two 1 MiB buffers per transfer; verification reuses them |
| All uploads and streaming reads | Eight combined globally (hard maximum), default two per shard; materializing blob GET uses these I/O slots too |
| Global streaming memory | 32 MiB reserved credits; no overcommit by queueing more chunks |
| Materializing GET | Separate configured byte/concurrency budget; up to 64 MiB result allocation is not covered by the streaming claim |
| Blob file quota | Default 8 GiB of charged file extents and 4,096 identities, including staging/orphans/history; separate from the normal Segment budget |
| Recovery metadata | Reserve `4 MiB + 512 * max_files` from the configured recovery budget before allocating blob maps/buffers; insufficient budget rejects open |
| Maintenance space | Reserve an additional 65 MiB of available space before staging, beyond the configured free-space floor |
| FDs / metadata | Transfer slots bound open blob FDs; global file/byte caps bound metadata and retained history |
| Per-turn work | Finite chunk, hash, completion, scan and GC batches; no whole-blob work on Reader/Writer |

The streaming cap covers new transfer-owned key/buffers/tasks, not pre-existing Segment/Index
capacity, caller buffers or returned owning values. Normal publication staging remains subject to
its existing Store limits. Credit exhaustion returns the existing appropriate resource/overload category or waits only on
the caller's bounded deadline. Reservations precede copying/dispatch; work cannot bypass credits
by moving bytes into an executor queue. Disk-full, FD exhaustion, slow disks, abandoned transfers
and slow readers must remain bounded. Metrics distinguish logical live bytes, historical roots,
staged/installed-orphan/pinned bytes, physical allocated bytes, active transfers, credits, I/O
queueing, checksum failures, GC debt and progress.

Single-item `Store::put/get/erase` in the opt-in profile must preserve logical bytes across inline
and blob values. Larger `put` materializes input supplied by its caller but stages it in bounded
slices; streaming is the bounded-memory caller alternative. Batch APIs, C ABI v1, daemon and SDK
entry points must explicitly reject unsupported blob operations before side effects until their
integration is separately proven. They must never expose descriptor bytes or silently truncate.

## 7. Backup, tools and future wire work

Before the embedded milestone gate can close, v2-aware inspect/verify and fenced backup/restore
must understand the entire descriptor closure. A snapshot pins every blob referenced by every
copied committed Segment, not just the latest keys. Copy and verify those files before publishing
the destination Manifest last; include directory creation/sync and keep cleanup away from pinned
source files. A rejected or interrupted backup is never promoted. Unsupported tools refuse v2
before mutation; repair cannot invent missing payloads. Offline migration copies complete values
into a new Store, re-encodes destination Store/owner identities and never reuses a source BlobRef
as authority in another Store. It must be separately implemented before advertising that tool on v2.

Wire v2 frame sizes, opcodes and official-client outcomes remain unchanged in this iteration.
A future ADR must specify capability negotiation, begin/chunk/commit/abort/read messages, per-
connection/principal/global credits, immutable owner binding, request sequencing, timeout and
disconnect outcomes, transfer expiration and bounded persistent idempotency/resolution. It must
define old-client behavior on a blob key before enabling blob-capable daemon Stores. There is no
64 MiB frame or automatic retry allocation in this proposal.

## 8. Acceptance matrix and implementation order

This is the full qualification matrix, not a claim that every row is closed. The implementation
has local codec/integration/lifecycle tests and an E2 crash matrix; native platform, exhaustive
fault, controlled performance and extended-soak qualification remain open. Requirements and
retained evidence pointers live in [large-blobs.yaml](../../engineering/requirements/large-blobs.yaml)
and [the blob gates](../../engineering/gates/large-blobs.yaml).

| Phase / matrix row | Required automated proof before advancing |
| --- | --- |
| A — codec and version boundary | Independent LE encoder/decoder and golden fixtures for BlobRef/file/footer and each v2 wrapper; all byte offsets, CRC and SHA vectors; truncation, trailing bytes, unknown versions, nonzero reserved bytes, overlap, offset overflow and mismatched Store/owner/ID |
| A — legacy rejection | Existing released binary refuses even empty v2 Stores read-only; future engine reads unchanged v1 fixtures; future required versions never trigger fallback or cleanup |
| B — bounded storage | Values 0, 1, 1,048,576, 1,048,577, 67,108,864 and 67,108,865; inline-fit boundary minus/at/plus one for several key sizes; maximum accepted key with 64 MiB value; expected-size underrun/overrun, unknown size and non-aligned writes |
| B — resource faults | EINTR, short/zero I/O, ENOSPC, EDQUOT, EIO, EROFS, FD/allocation exhaustion and sync/install failure; exact credit return after every terminal path; stalled transfer cannot exhaust unrelated inline admission |
| C — crash and restart | Process termination before/after ID-range reservation, each chunk, seal verification, file sync, install, directory sync, descriptor write/sync, commit-slot write/sync, generation publication and ACK; distinguish definitely acknowledged from indeterminate operations |
| C — policies | All strict acknowledged values survive E2; periodic flush survives, unflushed loss follows existing policy; no selected descriptor has a missing/partial payload; failures do not resurrect older values |
| D — logical integration | Inline→blob→inline, blob→blob, erase/TTL, same-key parallel uploads with intervening inline PUT/ERASE, independent owners, deterministic read-after-ACK and fail-closed publication faults |
| E — lifetimes / GC | Paused Reader, chunk I/O, generation retirement, overwritten/expired history, periodic dirty descriptors no longer in any generation, both compaction-intent Manifests, staged/install race, backup pins, GC crash/deletion retry, shutdown/reused-handle ABA; no delete before final root retires |
| E — operations | Descriptor-only reclaim benefit without Segment gain, early rotation under active-log blob debt and progress with upload quotas exhausted; backup→restore→verify with historical references, interrupted copy and corrupt/missing blobs; unsupported repair/migrate tools fail before modifying v2 |
| F — inline A/B and saturation | Same-source opt-in no-blob profile vs legacy path with identical workload/durability; allocation/CPU/RSS and p50/p95/p99/p99.9; hot-key/skew, slow consumers, disk saturation and compaction overlap; publish raw repeats and confidence/noise |
| G — future network | Separate accepted wire/SDK/ABI contracts, bounded parser/fuzz/interop and retry-resolution evidence; no enablement from embedded proofs alone |

Initial controlled-host acceptance targets for phase F are no more than 3% median throughput
loss and 5% p99 increase on declared inline workloads, with no added per-operation blob allocation
or payload hashing. These are acceptance budgets, not measured results. Predeclare repeat counts,
hardware, datasets and confidence method; overlap with measurement noise is inconclusive. Report
p99.9 and retained memory separately rather than hiding them in throughput. Any threshold change
requires the linked requirement and evidence to be reviewed.

The embedded milestone includes A–F, recovery, GC and backup safety; it is not just a file codec
demo. Native evidence remains platform-row specific across macOS, Linux, FreeBSD and OpenBSD.
Power-loss E3/E4 campaigns remain open regardless of a successful process-kill suite. Feature
implementation must not raise the project's architectural-prototype claim on its own.
