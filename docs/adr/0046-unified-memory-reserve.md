# ADR 0046: One memory reserve for a pageable Index and resident Segment images

- Status: proposed
- Date: 2026-11-14
- Deciders: storage, concurrency and performance maintainers
- Applies to: durable Stores (embedded and `glifistored`); persistence v1; wire v2
- Amends: ADR 0045 (budget, admission, eviction and read validation),
  ADR 0004 (Index memory may be paged), [residency model](../architecture/residency-model.md)
- Supersedes: none

## Context

ADR 0045 proposes resident Segment images with their own byte budget and mandatory active images.
Two further requirements change that design:

1. The Store must not stop when RAM is short. The Worker Index is mandatory derived state: it must
   keep growing, and the operating system must be able to page it out to swap or to a file.
2. Memory needs one operator-visible setting. Separate budgets for the Index, the ADR 0017 hot cache
   and Segment images cannot be reasoned about together.

The log already is the only durable truth (ADR 0003, ADR 0004). Everything in RAM is derived and can
be rebuilt or reread. A single reserve can therefore order every memory use by necessity.

## Decision drivers

- The Store never refuses a write because a RAM policy is exhausted; it degrades to paging.
- Resident durable reads cost a memory read, not a syscall, whenever the reserve allows.
- One configuration value, one order of priority, one eviction rule.
- No change to acknowledgement points, visibility, recovery authority or on-disk and wire bytes.
- No allocation after a persistent publication boundary; images are an optimization only.

## Alternatives considered

- **ADR 0045 as proposed.** Separate image budget; opening fails when it cannot hold one image per
  Worker. It treats images as mandatory and still lets the Index grow outside any policy.
- **Refuse Index growth beyond the reserve.** Bounds RAM, but turns memory pressure into write
  outages. Rejected by driver 1.
- **`mlock` the reserve.** Prevents paging of the hottest data but needs privileges and fails hard
  under pressure. Rejected.
- **Map the Segment files for reads.** Rejected by ADR 0045 (SIGBUS on I/O errors, unbounded page
  residency); unchanged.

## Decision

### 1. One reserve, ordered by necessity

`DurableResourceLimits::memory_reserve_bytes` (default 1 GiB) is the RAM the durable runtime keeps
resident for derived state. One `MemoryReserve` per Store accounts for two uses, in priority order:

1. **Index** (mandatory). Worker Index slot, control and heap-key arrays are charged unconditionally.
2. **Segment images** (optional). Admitted only while `index + images ≤ reserve`.

`image budget = max(0, reserve − index)`. Index growth shrinks the image budget; it is never refused.

### 2. Pageable Index

Large Index arrays (1 MiB or more) are anonymous mappings while the reserve still covers them. Above
the reserve:

- with `index_spill_directory` set, new arrays are shared mappings of an unlinked scratch file
  created there; the kernel pages them to that file instead of swap. The file is never synchronized
  and never reopened: the Index is rebuilt from the log at open (ADR 0004);
- otherwise they stay anonymous and the kernel may page them to swap.

Smaller arrays stay on the heap. Allocation failure keeps its existing `resource_exhausted` polarity.
Index storage built during recovery is charged when the runtime attaches the reserve.

### 3. Resident images (amends ADR 0045 §1)

- An image is an anonymous, lazily touched mapping of one Segment's size whose bytes equal the
  file's at every published offset.
- The Writer copies each encoded Record into the active image before the file append. No reference to
  that offset exists until the append succeeds and is published through the existing generation
  publication; a failed append leaves the bytes unreachable.
- An active image is optional. When it is not admitted, the Segment stays cold until it is sealed.
- A resident GET applies the same validation as a cold GET, including CRC32C, format version,
  sequence, size, key/hash and expiry. It skips only the positional read and the scratch buffer.
- At open, images are loaded for active Segments first, then sealed Segments by descending Segment
  ID, until the first refusal. A sealed image is charged for its committed bytes only.
- Compaction mirrors every output Record while it stages the output. An output may borrow up to the
  image bytes its sources hold; the sources stop counting when the transaction commits. Live-only
  data is therefore born resident.

### 4. Eviction (amends ADR 0045 §2)

Eviction runs only at a Worker's rotation, before any persistent write of that rotation:

- the Worker drops images of its own sealed Segments, oldest image first, until a new active image
  fits;
- dropping republishes the generation with the same shared file handle and no image, then advances
  the Worker's read catalog revision. Readers that already hold the old generation keep using its
  image; the pages return when the existing generation reclamation drops the last holder;
- an evicted image stops counting against the reserve immediately.

Because compaction outputs are created last, the oldest-first rule keeps live-only data longest. A
generation identity check that compared generation objects now compares their shared file handle, so
eviction does not abort a concurrent compaction.

### 5. Configuration

| Key | Default | Meaning |
| --- | --- | --- |
| `memory_reserve_bytes` | 1 GiB | RAM for derived state; 0 keeps every Segment cold |
| `resident_reads_enabled` | true | Use images when the reserve admits them |
| `index_spill_directory` | empty | Where Index arrays beyond the reserve are paged; empty means swap |

An `index_spill_directory` that is not an existing directory fails open with `invalid_argument`.

## Consequences

- Durable GETs on resident Segments read memory. The embedded path benefits immediately. The daemon
  still routes a prepared durable read through the ADR 0016 executor; serving resident reads inline
  on the Reader is a follow-up (ADR 0045 §3).
- Open reads the Segments it makes resident once more after recovery, up to the reserve.
- Index arrays of 1 MiB or more are mappings even in volatile Stores; AddressSanitizer no longer
  instruments overflows inside them (the guard is the mapping boundary).
- Memory pressure produces paging, not write refusal. Paged Index lookups are slower; the
  reserve is the operator's tool to keep them resident.
- A resident read does not re-detect on-disk damage that happens after the bytes were mirrored or
  loaded (HAZ-042). A spill file on a full or copy-on-write filesystem can raise `SIGBUS` when a page
  is first written; Linux and FreeBSD preallocate the file, macOS and OpenBSD do not (HAZ-043).
- The paired Reader index (`ImmutableReadIndex`) keeps its own anonymous mappings. They are
  swap-pageable but are not yet charged to the reserve or spilled to a file.
- Images are optional, so the exhaustive allocation-fault proofs run with images disabled for the
  mandatory path and separately prove that a failed image allocation leaves a committed, readable
  mutation or compaction.

## Compatibility and migration

Persistence v1, the blob profile, wire v2, the C ABI, acknowledgement points and client outcomes are
unchanged. New configuration fields have defaults; existing Stores open unchanged.
`max_resident_segment_bytes` from ADR 0045 is not introduced.

## Verification

Local evidence (architectural prototype, not release evidence):

- unit: reserve priority and overdraft, heap/anonymous/spill backing, unlinked spill files, image
  charge trimming, pageable-array growth, an Index spilled beyond a zero reserve stays exact;
- integration: resident reads across open, append, rotation and compaction; a zero reserve keeps
  everything cold without refusing writes; reopen with images enabled and disabled;
- cold-path tests (blocked reads, cancellation, post-open corruption) run with images disabled;
- allocation-fault enumeration for mutation, rotation and compaction, with and without images;
- the existing crash, fault and sanitizer suites.

Still required before acceptance: the ADR 0045 benchmark set, eviction concurrent with readers under
TSan, daemon inline resident reads, and charging the paired Reader index.

## References

- [ADR 0003](0003-log-indexed-storage.md), [ADR 0004](0004-index-as-derived-state.md),
  [ADR 0016](0016-bounded-cold-read-executor.md), [ADR 0017](0017-bounded-durable-hot-cache.md),
  [ADR 0044](0044-bounded-prefix-compaction-remap.md),
  [ADR 0045](0045-memory-resident-durable-reads.md)
- [Lean memory algorithm](../architecture/lean-memory-algorithm.md)
