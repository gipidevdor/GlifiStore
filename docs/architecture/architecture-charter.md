# Architecture charter

**Normative** · GlifiStore 0.1.x · reviewed 2026-08-26

The fixed decisions, safety rules and non-goals that every change respects. GlifiStore is a native,
many-core, memory-first key-value store: its fast path pays only for exact-key operations and must
remain measurable and resource-bounded.

## Fixed decisions

- One logical Store and one logical key-space.
- Workers (shard pairs) are sized automatically from the best platform topology and memory signal
  available. An explicit override is stable for the process lifetime and is required when exact
  affinity-aware sizing matters.
- The internal vocabulary is Store, paired Worker ownership, Index, Segment, Record and compaction.
  The supported API exposes no Worker, Index, Segment or shard abstraction.
- Every physical Segment is exactly 64 MiB (67,108,864 bytes).
- Records are variable-size, 8-byte aligned, append-only and immutable once published. They never
  span normal Segments.
- The Index maps a full logical key to a `RecordRef`. Its destination is a benchmark-selected flat
  open-addressing table, not `std::unordered_map`.
- A `RecordRef` is positional (stable IDs, offsets, sizes, sequence and generation) and never
  persists a process address.
- The Index is derived acceleration state. Records in Segments suffice to rebuild it; for one full
  key, the highest valid sequence defines visibility.
- Persistence v1 uses a versioned Manifest and alternating CRC-protected Segment commit slots. Bytes
  beyond the newest valid committed extent are never recovery input.
- Creating a durable Store persists the routing algorithm, Worker count and routing epoch. Reopen
  uses them until an explicit migration changes them.
- Paired Reader–Writer concurrency is the embedded default and the only daemon runtime. Each owner
  publishes immutable read generations and has exactly one mutation executor at a time. The
  embedded `legacy_mutex` escape hatch is deprecated in 0.1.x and removed in 0.2.
- Vacuum is copy-build-validate-publish-retire. It never rewrites a published Record in place.
- The Index and immutable read-generation metadata are RAM-resident. Volatile Segments are
  RAM-resident; durable Segments are file-backed and read through bounded exact-generation pins. The
  deprecated legacy hot cache is not a paired-read authority.
- Linux, macOS, FreeBSD and OpenBSD are architectural targets since 0.1.0; macOS is the primary
  development environment. All four have build and test workflows. Hosted and BSD VM rows are
  portability and regression evidence, not filesystem or device durability certification.
- Memory safety, integer safety, data-race freedom, corruption detection and bounded resource use
  are release requirements, not optional hardening.

The embedded Store implements persistence v1 with automated process-termination coverage. Release
criteria are the [quality gates](../production-readiness.md) and the
[persistence v1 roadmap](../v1-production-roadmap.md).

## Safety

GlifiStore is performance-oriented C++, not unchecked C++.

1. No raw owning pointer and no manual `new`/`delete` outside audited allocator internals.
2. No unchecked buffer, offset, capacity or allocation arithmetic.
3. No persisted bytes decoded through object-layout casts.
4. No unbounded allocation derived from untrusted metadata.
5. No non-owning view stored beyond its guaranteed source lifetime.
6. No Segment reuse without generation validation.
7. No unmap, unlink or reclaim while an active reader can hold a `RecordRef`.
8. No data race or undefined behavior accepted as harmless.
9. No parser or recovery reader without malformed-input tests and fuzz coverage.
10. No optimization that bypasses validation without a benchmark, an invariant proof and review.

Required verification: strict compiler warnings in CI; ASan with UBSan, and TSan jobs; fuzz targets
for Record decoding, Segment scanning and Index rebuild; a regression test for every memory-safety
or corruption issue found; and explicit errors for invalid extents, checksums, references, state
transitions and overflow.

Unsafe OS boundaries (`mmap`, file descriptors, platform topology calls) must live behind small RAII
wrappers or platform modules. Memory corruption or an allocator invariant failure is a fail-closed
process event: the runtime never pretends that continuing is safe.

## Performance contract

Exact-key operations target constant expected work relative to the total key count. The project
optimizes cache misses, allocations, copies, syscalls, probe count and tail latency, not asymptotic
notation alone. Background work is budgeted, and controlled rejection is preferred to unbounded
queuing.

## Non-goals

- Text-protocol or command-emulation compatibility layers.
- SQL, joins, generic secondary indexes and unbounded ordered scans.
- Performance claims without reproducible benchmarks and a hardware description.
- GPU or NPU dependencies in the key-value fast path.
- Unversioned disk or wire changes, or serialization through native object layout. Persistence v1
  and wire v2 evolve only through explicit codecs, compatibility rules and fixtures.
