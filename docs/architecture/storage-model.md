# Storage model

**Descriptive** · paired Store, volatile and persistence v1 backends · reviewed 2026-08-26

How a Store organizes keys, Records, Segments and the Index, and how it reclaims space. The
normative component map is the [architecture specification](../spec/architecture.md).

## Structure

```text
Store
  ShardPairRuntime (default concurrency)
    owner -> immutable ReadGeneration + one mutation executor
    volatile backend -> mutable Index + in-memory Segments
    or durable backend -> mutable Index + active file Segment + generation pins
  control/maintenance
    GlobalSegmentManager or DurableRuntimeCatalog
```

The Store presents one key-space. Routing chooses a Worker deterministically from the key hash. The
Worker owns mutation of its Index partition and appends immutable Records to its active Segment.

The volatile `Worker` and the durable runtime Worker are separate backends under the same paired
ownership rule. The global manager or catalog is control plane: an ordinary paired lookup stays in
the owner Reader's immutable generation and never scans another Worker. Mutable backend Index state
is Writer-only during normal paired operation.

## Read and write paths

```text
read:  key -> owner -> immutable ReadGeneration -> RecordRef/pin -> Segment -> value
write: key -> owner mutation executor -> encode -> append -> publish backend Index + ReadGeneration
```

Publishing a coherent `ReadGeneration` makes a new Record visible to paired readers. Replacing or
removing an Index entry updates Segment liveness accounting; the old Record stays physically present
until its Segment is reclaimed or compacted.

In `durable_sync`, a successful sync of the alternate Segment commit slot is the durable commit
point, and it precedes in-memory publication. The full ordering and acknowledgement rules are in
[durability and recovery](durability-recovery.md).

## Worker Index

The Store has one logical Index, physically partitioned by Worker ownership. Design text calls it
`Index`, not `KeyIndex`.

```text
full key -> RecordRef(segment_id, offset, size, sequence, generation)
```

Each partition is a SwissTable-style flat open-addressing table: group control bytes, hash
fingerprints, a 7/8 load limit, cached 32-bit hash tags, inline keys up to 24 bytes and a long-key
arena. Every slot is exactly 64 bytes on supported 64-bit targets; inline storage and the heap
offset share one union, and the inline mode occupies the high bit of the packed key length. Hash
tags only filter comparisons: complete key bytes remain the collision-safety authority, and
rehashing reconstructs the full hash from them. The algorithm is specified in
[Index v1](../spec/index-v1.md); the rationale is [ADR 0007](../adr/0007-swiss-table-index.md).

Live and deleted controls are counted separately. Capacity policy uses their effective occupancy,
reuses deleted slots and rebuilds transactionally at the same capacity when tombstones dominate.
Durable `erase_no_compact()` stays allocation-free and defers maintenance to a prepared insertion.
Index statistics expose effective load, tombstones, observed probe groups and rebuild counters.

The Index owns a copy of every key: up to 24 bytes inline, longer keys in `KeyArena`. A
reference-based layout is permitted only after its lifetime, vacuum publication and cache effects
are proven. Layout and SIMD changes must be justified on GlifiStore workloads: key sizes of 8, 16,
32, 64 and 256 bytes; hit and miss ratios; insert, replace and delete mixes; millions of entries;
iteration during vacuum; memory per entry; and p50, p99 and p99.9 probe and operation costs.

## Recovery

Scanning valid Records reconstructs the Index. Recovery compares full keys, never hashes alone. A
higher sequence supersedes a lower one whatever the Segment scan order. A newest tombstone removes
visibility; a newest expired Record is left out of the rebuilt Index.

Persistence v1 has no persisted Index checkpoint. A future derived checkpoint must stay discardable
and allow fallback to validated Segment scanning.

Durable recovery uses the persisted routing algorithm, Worker count and routing epoch. Machine
topology never silently repartitions an existing Store on reopen.

## Vacuum

Vacuum is copy-build-validate-publish maintenance over Index-reachable Records.

```text
current {Index, Segment set}
       -> copy visible Records into new fixed Segments
       -> build a new Index
       -> validate referential and record integrity
       -> atomically publish the new pair
       -> retire old Segments
       -> reclaim after readers and retention obligations are gone
```

The volatile runtime uses the single-threaded builder under the selected Worker's mutex. It copies
only Index-visible Records owned by selected sparse sealed Segments, builds the complete replacement
Index before publication, validates aggregate source liveness and commits the replacement catalog
without allocating after source liveness changes. Existing snapshots provide the reclamation epoch:
retired source bytes stay alive until their last shared owner releases them.

Reclaiming a whole Segment is cheaper than compaction. A sealed Segment with no live Record and no
retention or reader obligation may be retired without scanning its Records. A partially live Segment
becomes a vacuum candidate by live-byte ratio and reclaim benefit. Durable Stores compact through
[durable compaction](durable-compaction.md).

No vacuum operation may resurrect a lower-sequence Record, mutate a published Record, delete a
Segment a reader can still address, or perform unbounded work in a latency-critical loop.

## Complexity

| Operation | Expected cost |
| --- | --- |
| Index lookup, insert, replace, erase | O(1), plus O(key length) hashing and comparison |
| Positional Record access | O(1) |
| Segment append and rotation | O(1), excluding Record bytes copied |
| Full Index rebuild | O(Records scanned) |
| Full vacuum rebuild | O(live Index entries and live bytes copied) |
| Whole dead-Segment reclaim | O(1) logical retirement, plus OS and storage release costs |
