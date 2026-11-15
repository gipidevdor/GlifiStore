# ADR 0045: Memory-resident durable reads

- Status: proposed
- Date: 2026-11-14
- Deciders: storage, concurrency and performance maintainers
- Applies to: paired runtime durable Stores (embedded and `glifistored`); persistence v1; wire v2
- Amends: ADR 0016 (cold reads per connection), ADR 0017 (paired mode no longer depends on the hot
  cache), [residency model](../architecture/residency-model.md)
- Supersedes: none

## Context

GlifiStore is memory-first, yet a durable GET does not read memory. In the paired runtime,
`prepare_published_get` always yields a borrowed cold read: every GET performs a positional `pread`
of the Segment, a CRC32C check, a key/hash check and a fresh scratch allocation. The ADR 0017 hot
cache is consulted only by the non-paired mutex runtime. In `glifistored`, a cold read is handed to
the ADR 0016 executor and the connection stops decoding until it completes, so pipelining gives
almost nothing.

Local campaign `benchmarks/results/local-macos-2026-08-30-full-b971a15` (Apple M4, Release,
16-byte keys, 64-byte values; advisory, not release evidence):

| Path | Volatile | Durable |
| --- | ---: | ---: |
| Embedded GET, 1 thread | 4.25 M/s | 0.35 M/s (`durable_sync`) |
| Daemon GET-only, 1 pair, pipeline 1 / 8 / 32 | 44 k / — / 885 k/s | 19 k / 32 k / 40 k/s |
| Daemon GET-only, 4 pairs, pipeline 32 | 2.71 M/s | 82 k/s, p50 763 µs |

Durability concerns when a mutation is acknowledged and what survives a crash. It does not require
reads to come from the file. The read cost is therefore an implementation choice, not a contract.

## Decision drivers

- A resident durable GET costs the same as a volatile GET: no syscall, lock, CRC or thread hop.
- Unchanged acknowledgement points, visibility, RAW, recovery authority and on-disk/wire bytes.
- Bounded, configurable memory; data larger than the budget stays correct through the cold path.
- No new reader lifetime hazard: residency changes reuse the existing generation reclamation.
- One portable design on macOS, Linux, FreeBSD and OpenBSD.

## Alternatives considered

- **Enable the ADR 0017 hot cache in the paired path.** Per-value admission, a Worker-mutex
  snapshot and a second copy of every hot value; it keeps a lock on the read path and duplicates
  bytes the Writer already produced. Rejected.
- **`mmap` the Segment files.** Zero copy for sealed Segments, but I/O errors and truncation surface
  as `SIGBUS` instead of fail-closed status, `write`/`mmap` coherency for the active Segment is not
  uniform across the supported BSDs, and page residency is controlled by the kernel rather than an
  explicit budget. Rejected for the value-access contract; may be reconsidered for sealed Segments
  only.
- **Keep file reads and make them cheaper** (scratch reuse, `io_uring`, more executor threads).
  Removes a fraction of the cost; every GET still pays a syscall and, in the daemon, a thread hop.
  Kept only for the cold path.

## Decision

### 1. Resident Segment images

Each durable Segment generation may own a **resident image**: an anonymous, lazily touched 64 MiB
mapping with the exact bytes of its Records.

- **Active Segment.** The Writer encodes each Record into the active image and writes the file from
  those same bytes. The image is always resident while the Segment is active.
- **Publication.** The read generation is published only after the image bytes are written. The
  existing generation publication (release/acquire) orders them for every Reader. Acknowledgement
  and visibility points are those of the selected durability mode, unchanged.
- **Read.** A published Record view carries its generation pin; when the pin has an image, GET
  decodes the Record from memory exactly as the volatile path does, with key and hash comparison.
  No CRC is recomputed: the bytes come from the Writer's own encoding or from recovery validation.
- **Sealed Segments and compaction outputs** keep their image while the budget allows. Compaction
  builds the output image while staging when budget is available; otherwise the output starts cold.
- **Recovery** already reads and validates every Record to rebuild the Index (ADR 0004). It loads
  images newest-first while the budget allows; the remainder starts cold.

### 2. Budget and eviction

`StoreConfig::max_resident_segment_bytes` (default 1 GiB) bounds image bytes for the Store, with
high/low watermarks at 100% / 90%. Active images are mandatory and counted; opening a Store whose
budget is smaller than `worker_count × 64 MiB` fails with `invalid_argument`.
`resident_reads_enabled = false` disables images and restores the current cold behavior.

When the high watermark is reached, the Writer evicts the oldest sealed images until the low
watermark. Eviction clears the pin's image pointer and retires the mapping into the same
epoch/lease reclamation that retires generations: the mapping is unmapped only after every reader
that could have observed it has quiesced. An image allocation failure is correctness-neutral: the
Segment is simply cold.

### 3. Cold reads

A GET on a non-resident Segment keeps the current pinned path: positional read, CRC32C, key/hash and
expiration checks, fail-closed on mismatch. Its scratch buffer is reused per thread or per executor
slot instead of allocated per call.

In `glifistored`, a connection may keep up to `kMaximumMutationWindow` (32) cold reads in flight.
Each admitted request reserves an ordered response slot and its maximum response bytes against the
existing connection output watermark; responses are emitted strictly in request order. Executor
capacity, cancellation and connection lifetime rules of ADR 0016 are unchanged.

Implementation rules:

- The first cold read of a connection opens a window of up to 32 ordered slots. While the window is
  open, the connection parses only GETs; any other opcode waits until the window drains, as a GET
  waits for an open mutation window (ADR 0037).
- Every GET in the window takes a slot. A response decided at once (resident hit, not found,
  refusal) is held in its slot; a cold read reserves its header plus the smaller of its Record size
  and the value budget. The Reactor's disk-read lane is FIFO, so completions arrive in slot order,
  and the decided prefix of the window moves to the output buffer.
- Output bytes plus slot reservations never exceed the output watermark. A GET that does not fit,
  or finds the disk-read lane or lease table full, stays unparsed until the next completion of the
  same connection. It is not refused: only the first cold read of a connection is answered
  `OVERLOADED` when the lane is full, as before.
- Closing the connection cancels every queued read of the window; late completions find a stale
  token.
- STATS reports `cold_reads_pipelined` and `cold_read_window_deferrals`.

### 4. Out of scope

Public reads remain owning copies (ADR 0009). Per-GET admission and lease overhead, the blob-profile
snapshot lock and a borrowed-read API are separate decisions.

## Consequences

- Resident durable GET becomes a memory read; the daemon serves it on the Reader without an
  executor hop, so pipelining applies again.
- Memory grows with resident data up to an explicit budget, visible in `--dump-config` and STATS.
  Segment images are 64 MiB granular; only touched pages are resident.
- A resident read does not re-detect later on-disk corruption. Detection of media damage moves to
  recovery, compaction, backup verification and the cold path. This is recorded as a hazard.
- In paired mode the ADR 0017 hot cache becomes redundant and is not allocated. The non-paired mutex
  runtime keeps it unchanged.
- Recovery memory now includes images, bounded by the same budget.

## Compatibility and migration

No change to persistence v1 bytes, the opt-in blob profile, wire v2, the C ABI, acknowledgement
points or client outcomes. New configuration keys have defaults; existing Stores open unchanged. The
residency model is updated to define `VALUE HOT` as a resident image read.

## Verification

Required before this ADR moves to accepted-and-implemented (requirement `GS-PERF-DREAD-001`, gate
`GATE-DURABLE-READS`, hazard `HAZ-042`):

- RAW and visibility litmus for every durability mode, with reads on resident, evicted and
  re-loaded Segments.
- Eviction concurrent with readers under ASan and TSan; no access to an unmapped image.
- Recovery with budget smaller than data; crash matrix unchanged (no format change).
- Allocation-fault injection on image creation falls back to cold reads.
- Daemon: ordered responses with mixed resident and cold GETs, cancellation on close, output
  watermark respected.
- Benchmarks, same host and shape under the [benchmark standard](../spec/benchmark-standard.md):
  - embedded resident durable GET ≥ 90% of volatile GET;
  - daemon resident durable GET-only at pipeline 32 ≥ 80% of volatile;
  - fully cold daemon GET throughput increases with pipeline depth;
  - PUT throughput not regressed beyond run-to-run noise; RSS reported against the budget.

## References

- [ADR 0004](0004-index-as-derived-state.md), [ADR 0009](0009-public-read-ownership.md),
  [ADR 0016](0016-bounded-cold-read-executor.md), [ADR 0017](0017-bounded-durable-hot-cache.md),
  [ADR 0031](paired-reader-writer-shards.md), [ADR 0032](0032-paired-concurrency-embedded-store.md)
- [Persistence v1](../spec/persistence-v1.md),
  [concurrency and memory model](../spec/concurrency-memory-model.md),
  [residency model](../architecture/residency-model.md)
