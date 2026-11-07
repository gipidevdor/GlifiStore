# ADR 0039: Pre-intent staged compaction output

- Status: accepted
- Date: 2026-08-27
- Deciders: persistence and performance maintainers
- Applies to: durable whole-Worker compaction, persistence format v1
- Amends: ADR 0015, ADR 0023
- Amended by: ADR 0040, ADR 0043
- Supersedes: none

## Context

ADR 0015 originally built replacement Segments after publishing the compaction intent. The runtime
therefore held the Store-wide Manifest publication lease across preallocation, every Record copy,
output synchronization, sealing and validation. No Worker or catalog mutex was held during that
I/O, but an unrelated rotation still had to wait for the lease. The real head-of-line blocking was
only moved from a mutex to a logical catalog exclusion window.

The v1 intent admits exactly two Manifest authorities, `Mold` and `Mnext`. It cannot coexist safely
with a third Manifest generation. The expensive output build does not need that dual-authority
state: before intent publication `Mold` remains the sole authority and a replacement under a private
temporary name is disposable.

The builder also retained a complete owning `IndexEntry` vector and duplicated every live source
entry inside its placement vector. That inflated transient metadata during an already memory-heavy
maintenance operation.

## Decision drivers

- Preserve deterministic fail-closed recovery and exact v1 byte compatibility.
- Remove Segment-sized copy I/O from the global Manifest publication lease.
- Never use a `RecordRef`, file, or Segment without an immutable generation pin or private ownership.
- Bound and expose queueing-critical duration and transient metadata.
- Avoid a new journal format or a third recoverable Manifest authority.

## Alternatives considered

- **Keep post-intent output creation:** simplest recovery story, rejected because unrelated rotation
  latency remains proportional to all compaction copy I/O.
- **Release the lease while an intent exists:** rejected because rotation could publish a third
  authority not representable by intent v1.
- **Add a prepare intent or persistence v2:** permits richer coordination, but is unnecessary for
  private output names and would add format/migration risk.
- **Copy the complete Index or source metadata per output:** rejected for avoidable peak memory and
  cache traffic.
- **Incremental/tiered compaction transaction:** useful future work, but it changes the atomic
  tombstone domain and needs a separate ADR and recovery proof.

## Decision

One whole-Worker transaction uses this ordering:

```text
pin Mold + sealed source generations; copy owning Index snapshot
  -> release Worker/catalog synchronization
  -> scan and verify source Records
  -> construct the complete replacement Index and exact layout
  -> create .segment-<id>-<generation>.glifi.tmp outputs
  -> copy, synchronize, seal and verify every staged output
  -> acquire the Manifest publication lease; revalidate Mold
  -> publish and directory-sync the exact v1 compaction intent
  -> rename every planned staged output to its canonical final name
  -> synchronize the directory once
  -> validate target Worker sequence, batch state and generation pins
  -> publish and directory-sync Mnext
  -> install the prepared in-memory Index/catalog state
  -> retire Mold source names and synchronize the directory
  -> remove the intent and synchronize the directory
  -> final namespace audit; release the publication lease
```

Before the intent is durable, staged names are never authoritative, are not referenced by `Mold`,
and may be removed after a complete authoritative recovery scan. The staged file handle has private
health state: a failed disposable copy cannot poison the authoritative `DataDirectory`. A RAII guard
removes staged names on an orderly pre-intent failure.

After the intent becomes durable, failure is `recovery_required`. Promotion first validates every
staged file as an exact sealed planned identity and rejects any existing canonical target. Renames
are descriptor-relative and followed by one directory sync. A partial rename or uncertain sync
poisons the current directory instance; restart validates `Mold` and removes both exact temporary
and canonical replacement names.

The placement vector stores an index into the owning source snapshot instead of a second owning
`IndexEntry`. It is constrained to at most one 64-byte cache line per live copied Record. Public
compaction results report pre-intent duration, publication-lease duration, and a conservative
transient-metadata lower bound. The lower bound deliberately excludes allocator headers and heap
storage owned by copied strings; an allocation census remains required for total peak RSS.

Staged creation is exclusive and never removes a pre-existing temporary. A preparation transaction
tracks only the outputs it created, so an orderly rollback cannot remove a name owned by another
concurrent low-level builder. The public Store still rejects concurrent compaction before planning;
this lower-level rule is a defence-in-depth ownership invariant.

## Consequences

Unrelated rotations can commit while the compactor performs its dominant scan/copy/seal work. The
remaining lease is still a whole v1 transaction and includes intent publication, name promotion,
Worker quiescence, Manifest publication, source retirement, intent removal and final audit. Slow
filesystem metadata or a large retirement/audit remains visible as tail latency.

ADR 0043 replaces the selected-Worker sequence equality restriction with a bounded concurrent Index
delta, including group and TTL publications. Overflow or changed Manifest/source identities still
aborts safely. ADR 0040 adds bounded physical-write pacing inside private staging. A shorter cleanup
protocol and compaction across concurrent rotations remain deferred.

Automatic recovery now removes only recognized private engine temporaries after the complete
authoritative scan and synchronizes that cleanup. Unsafe, malformed, linked, canonical or unlisted
final entries still fail closed.

## Compatibility and migration

Record, Segment, commit-slot, Manifest and compaction-intent bytes remain persistence v1. Wire v2,
acknowledgement points, key routing, sequence allocation, visibility and public mutation outcomes do
not change. Existing Stores require no migration. The additional `CompactionResult` fields are
diagnostic source-level C++ API additions and do not alter the stable C ABI.

## Verification

- Segment-file unit tests prove private creation, sealed promotion and discard.
- Builder tests prove pre-intent copy failure leaves `Mold` healthy and no residue.
- Promotion failure after durable intent must be `recovery_required` and roll back on reopen.
- A blocked staged Record copy must allow an unrelated forced rotation to commit without waiting on
  the compaction lease.
- Online I/O-fault and process-kill matrices cover pre-intent staging, intent, partial promotion,
  Manifest authority, retirement and final cleanup.
- Wave 3: `storage_exhausted` fault matrices and write-amplification / temporary-space budget
  rejection before intent (`GS-PERSIST-AMP-001`); paced staging faults remain pre-intent. Platform
  evidence path placeholders live under `engineering/evidence/platform-durability/` — **not** E3/E4
  certification.
- Compaction benchmarks retain the measured pre-intent and publication-lease durations plus the
  transient-metadata lower bound.
- ASan, TSan, the complete test matrix and assurance referential validation remain required before
  accepting evidence beyond the local platform row.

## References

- [ADR 0015](0015-whole-worker-compaction.md)
- [ADR 0023](0023-maintenance-controller.md)
- [Crash-safe durable compaction](../architecture/durable-compaction.md)
- [Persistence format v1](../spec/persistence-v1.md)
- [Recovery state-transition matrix v1](../spec/recovery-state-matrix-v1.md)
