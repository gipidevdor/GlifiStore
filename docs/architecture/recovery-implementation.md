# Durable recovery implementation

**Descriptive** · persistence v1 recovery orchestrator, embedded durable Store

How the internal recovery orchestrator turns one locked, validated data directory into a
deterministic Manifest-aligned catalog, one rebuilt Index per Worker, restored next sequences and
explicit rotation requirements. After completing only validated bootstrap, compaction or rotation
intents, the public durable Store moves this state into the
[durable runtime catalog](durable-runtime-catalog.md).

The restart outcome of every intermediate bootstrap, commit and flush, rotation and compaction state
is normative in the [recovery state-transition matrix v1](../spec/recovery-state-matrix-v1.md).

## Recovery pipeline

`recover_durable_state` performs no repair, publishes no files and removes only recognized private
temporaries:

```text
read and decode authoritative manifest
  -> enumerate the locked directory and apply the read-only namespace policy
  -> group canonical catalog indexes by owner Worker
  -> for each Worker
       -> open one listed Segment
       -> verify complete encoded identity and selected commit
       -> validate manifest/commit lifecycle
       -> scan exactly the committed extent
       -> validate key hash and routing ownership
       -> retain the highest sequence decision for each full binary key
       -> close the Segment
       -> build that Worker's Index and restore maximum_sequence + 1
  -> with no compaction intent: remove recognized stale temporaries, sync the directory, re-audit
  -> return state only after every Worker and Segment succeeds
```

Manifest decoding has already established supported versions, one active Segment per Worker,
strict Segment-ID ordering and bounded catalog sizes. A missing listed file is promoted from
`not_found` to corruption, because the Manifest is recovery authority. The header's Store ID,
Segment ID, generation and owner must all match the catalog; filenames alone are never trusted.

The namespace audit reports exact private crash temporaries without deleting them; after every
Worker scan succeeds, ordinary recovery removes them, synchronizes the directory and audits again
([ADR 0039](../adr/0039-pre-intent-staged-compaction-output.md)). Unlisted final Segments,
malformed engine names, unknown entries, unsafe filesystem objects and missing catalog files reject
normal recovery before any Segment scan. The classification, the descriptor-relative algorithm, its
limits and the operator quarantine tooling are in the
[data-directory namespace audit](namespace-policy.md).

## Bounded scan and resources

The Segment layer exposes an internal visitor over one contiguous read of
`[4096, committed_end)`. Each Record is structurally decoded and checksum-verified once, and
recovery consumes its short-lived view directly: one scan, no per-Record positional re-read.

Catalog grouping uses a prefix-sum offset table plus one contiguous array of Manifest indexes. It is
`O(segment count + Worker count)`, makes no per-Segment allocation, and preserves canonical catalog
indexes in the output. Recovery deliberately processes one Worker and one Segment descriptor at a
time:

- file-descriptor usage is constant rather than proportional to catalog size;
- the temporary latest-key map exists for only one Worker;
- the scan buffer is bounded by the fixed 64 MiB Segment size;
- final per-Worker Indexes accumulate because they are the required recovery result.

Open applies `max_recovery_memory_bytes` before allocating the catalog grouping arrays and before
each new latest-key entry. The estimator charges fixed catalog and Worker components plus
conservative map, key-copy and final-Index storage. `max_live_keys` is divided into deterministic
Worker partitions; a recovered partition over its share rejects open before runtime service.

Expected rebuild time is `O(committed bytes + Records + visible-key hashing)`. Peak temporary memory
is `O(one committed Segment + distinct keys for the largest Worker partition + catalog grouping)`;
the returned Index and selected-commit catalog are additional required state.

Recovery is serial. Parallel Worker scans may be added only with bounded I/O concurrency and a
measured benefit on the target filesystem; unrestricted parallel scanning would amplify memory,
descriptors and random I/O.

## Record and visibility invariants

For every committed Record, recovery requires:

- a canonical Record extent and a valid CRC32C;
- a non-zero sequence, strictly increasing within its Segment;
- a first sequence strictly greater than the preceding non-empty Manifest-ordered Segment owned by
  the same Worker;
- a persisted key hash equal to the Manifest routing hash over the complete binary key: FNV-1a-64
  or keyed SipHash-2-4 ([ADR 0030](../adr/0030-keyed-worker-routing.md));
- `hash % persisted_worker_count` equal to the Segment owner;
- a reference Segment ID and generation equal to the opened file.

Filesystem enumeration never determines recovery order. Within canonical Manifest order, the
non-empty per-Worker Segment sequence ranges must be globally strict and non-overlapping; the
highest sequence for each exact key then wins. Equal or reversed cross-Segment ranges are
corruption, and equal winning sequences for one key are a conflict.

A newest tombstone or newest expired Record suppresses older values. It is never skipped early in a
way that could resurrect history. Sequence restoration uses the maximum committed sequence across
all of a Worker's Records, tombstones and expired decisions included. `UINT64_MAX` fails read-write
recovery instead of wrapping to zero.

The Index receives the already verified key hash, which avoids hashing the key again on insertion.
Hash equality never substitutes for full-key equality.

## Lifecycle matrix

| Manifest role | Selected persisted state | Recovery result |
|---|---|---|
| sealed | sealed | accepted |
| sealed | active | corruption |
| active | active | accepted as writable candidate |
| active | sealed | accepted only as the documented interrupted-rotation transition; Worker is marked `active_requires_rotation` |

The recovery scan does not append to a sealed active Segment. The durable runtime treats it as an
intent marker and completes only the exact next-identity pristine replacement transition before
serving writes.

## Returned state

The returned Segment-state vector is aligned index for index with `manifest.segments`; catalog
identity is not duplicated. Each Worker state holds its rebuilt Index, next sequence, Manifest
active Segment and rotation flag. Segment descriptors are closed after scanning. The owning Store
keeps the locked `DataDirectory` and reopens backing files through verified identities in the
integrated durable runtime catalog.

`DurableRuntimeCatalog` consumes this state without copying Index keys, keeps the directory lock,
serves CRC-verified owning reads through one immutable generation pin per catalog Segment, each
holding a read-only descriptor, and performs preflighted durable Record, slot and Index mutations
through at most one mutable active-Segment descriptor per Worker. It completes a sealed-active
marker only by creating or validating the exact pristine next Segment and durably publishing the
next Manifest.

Integration tests cover multi-Worker rebuild, cross-Segment newest-value selection, tombstones,
expiration, sequence restoration, lifecycle transitions, missing files, Store-ID mismatch, stored
hash mismatch, wrong-Worker routing, equal winning sequences, sequence exhaustion, crash temporaries
and unlisted-Segment rejection without adoption.

Open release work: operator quarantine and repair for arbitrary orphans; daemon asynchronous
durable-I/O completion; native-platform process-kill, disk-full and power-loss matrices. See
[production readiness](../production-readiness.md).
