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

1. **Index** (mandatory). Worker Index slot, control and heap-key arrays, and every paired Reader
   index base (records, lookup tables and key blocks), are charged unconditionally.
2. **Segment images** (optional). Admitted only while `index + images ≤ reserve`.

`image budget = max(0, reserve − index)`. Index growth shrinks the image budget; it is never refused.

### 2. Pageable Index

Large Index arrays (1 MiB or more) are anonymous mappings while the reserve still covers them. Above
the reserve:

- with `index_spill_directory` set, new arrays are shared mappings of an unlinked scratch file
  created there; the kernel pages them to that file instead of swap. The file is never synchronized
  and never reopened: the Index is rebuilt from the log at open (ADR 0004);
- otherwise they stay anonymous and the kernel may page them to swap.

Smaller arrays stay on the heap and are charged too. Allocation failure keeps its existing
`resource_exhausted` polarity. Index storage built during recovery is charged when the runtime
attaches the reserve.

Every mapping, Index or image, has a `PROT_NONE` guard page on each side and maps its whole
addressable size; an image's charge may be smaller (a sealed Segment's committed bytes). Under
AddressSanitizer, Index regions are heap regions so that overflows stay instrumented; images stay
mapped.

The paired Reader index uses the same regions. A Reader base lineage is created with the Store's
reserve, and every base derived from it (Delta merges, incremental merges, durable snapshot
replacements) inherits it with its one retired spare region, which stays charged while it is kept.

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
- Compaction mirrors every output Record while it stages the output. An output is admitted with
  the image bytes its sources still hold as overdraft, so the images left once the sources retire
  fit the budget; the sources stop counting when the transaction commits. Live-only data is
  therefore born resident.

### 4. Eviction (amends ADR 0045 §2)

Eviction runs only at a Worker's rotation, before any persistent write of that rotation:

- the Worker drops images of its own sealed Segments, oldest image first, until a new active image
  fits;
- dropping republishes the generation with the same shared file handle and no image, then advances
  the Worker's read catalog revision. Readers that already hold the old generation keep using its
  image; the pages return when the existing generation reclamation drops the last holder;
- an evicted image stops counting against the reserve immediately. When it is a source of the
  Worker's in-flight compaction, its bytes leave that compaction's credit at the same moment, under
  the lock that output admission takes, so an output never borrows bytes already returned.

Because compaction outputs are created last, the oldest-first rule keeps live-only data longest. A
generation identity check that compared generation objects now compares their shared file handle, so
eviction does not abort a concurrent compaction.

### 5. Resident reads complete where they are prepared (amends ADR 0045 §3)

A read of a resident generation never blocks: it is a bounded memory decode (a normal Record is at
most 1 MiB). It therefore needs no disk-read lane, cancellation token or pinned completion.

- `prepare_published_get` completes the read and returns a value when the pinned generation is
  resident and the Record is no larger than the caller's inline limit. The daemon Reader passes its
  connection response budget, so a Record that would not fit keeps the ADR 0016 path and its
  `record_too_large` outcome. Embedded callers pass no limit.
- Validation, expiry and error polarity are those of `complete_get`; only the hop is removed.
- A resident GET no longer occupies disk-read queue capacity, so it is not refused as `overloaded`
  when that queue is full. Cold GETs are unchanged.
- The owned cold path relinearizes on the on-disk generation (the shared file handle), not on the
  generation object, so rotation and image eviction do not force a retry. The bytes at a RecordRef
  of one file never change.

`DurableGetPathWorkerStats::resident_reads` counts reads decoded from an image.

### 6. Configuration

| Key | Default | Meaning |
| --- | --- | --- |
| `memory_reserve_bytes` | 1 GiB | RAM for derived state; 0 keeps every Segment cold |
| `resident_reads_enabled` | true | Use images when the reserve admits them |
| `index_spill_directory` | empty | Where Index arrays beyond the reserve are paged; empty means swap |

An `index_spill_directory` that is not an existing directory fails open with `invalid_argument`.

## Consequences

- Durable GETs on resident Segments read memory on the calling thread, embedded and daemon alike.
  The ADR 0016 executor serves only cold Segments and Records above the inline limit.
- An image page that the kernel has paged out under memory pressure faults on the Reader thread,
  exactly like a paged Index page. The reserve is the operator's tool to keep both resident.
- Open reads the Segments it makes resident once more after recovery, up to the reserve.
- Index arrays of 1 MiB or more are mappings even in volatile Stores, bounded by guard pages.
  AddressSanitizer builds keep them on the heap, so they do not exercise spill files.
- Memory pressure produces paging, not write refusal. Paged Index lookups are slower; the
  reserve is the operator's tool to keep them resident.
- A resident read does not re-detect on-disk damage that happens after the bytes were mirrored or
  loaded (HAZ-042). A spill file on a full or copy-on-write filesystem can raise `SIGBUS` when a page
  is first written; Linux and FreeBSD preallocate the file, macOS and OpenBSD do not (HAZ-043).
- Two paired Reader bases can coexist during a rebuild, and both are charged: a rebuild near the
  reserve shrinks the image budget until the old base is retired.
- `MemoryReserveStats::image_evictions` counts evicted images.
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
  charge trimming, guard pages and full-size mappings, pageable-array growth, an Index spilled
  beyond a zero reserve stays exact, a Reader base lineage charges and releases the reserve, a
  compaction credit shrinks when a source is evicted and then refuses an output it would have
  admitted;
- integration: resident reads across open, append, rotation and compaction; a zero reserve keeps
  everything cold without refusing writes; reopen with images enabled and disabled; the paired
  Reader completes a resident read at prepare and keeps the cold path above its inline limit;
- concurrency: with a reserve that holds one active image, 48 forced rotations evict sealed images
  while four reader threads read every published key, through the catalog and through paired
  Readers; in the catalog run a thread compacts continuously, so evictions withdraw sources of
  in-flight compactions. Values stay exact, evictions and compactions are observed, images end
  within the budget, and TSan and ASan report nothing (three runs each, local);
- cold-path tests (blocked reads, cancellation, post-open corruption) run with images disabled;
- allocation-fault enumeration for mutation, rotation and compaction, with and without images;
- the existing crash, fault and sanitizer suites.

- benchmark: `glifistore_benchmarks --filter store-durable-get` with and without `--cold-reads`
  (local numbers in the lean memory note; not release evidence).

Still required before acceptance: the full ADR 0045 benchmark set including the daemon, and the same
concurrency proofs retained as CI evidence on every platform row.

## References

- [ADR 0003](0003-log-indexed-storage.md), [ADR 0004](0004-index-as-derived-state.md),
  [ADR 0016](0016-bounded-cold-read-executor.md), [ADR 0017](0017-bounded-durable-hot-cache.md),
  [ADR 0044](0044-bounded-prefix-compaction-remap.md),
  [ADR 0045](0045-memory-resident-durable-reads.md)
- [Lean memory algorithm](../architecture/lean-memory-algorithm.md)
