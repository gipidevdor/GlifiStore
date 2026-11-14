# Durable compaction

**Normative** · persistence v1 durable compaction

The invariants of crash-safe durable compaction in persistence v1: which Segments one transaction
may replace, the order in which it publishes and retires them, and the only states recovery may
choose after a crash. The page also describes how the implementation meets them and what evidence
covers it.

The tree implements the deterministic planner and its resource gates; a checksummed intent codec
that embeds both exact Manifest authorities; descriptor-relative intent publication and removal; and
restart recovery of an interrupted transaction. The durable builder copies, seals and revalidates
private staged outputs for the selected sealed prefix, then installs the intent and promotes those
outputs; it also supports zero-output retirement. The internal durable runtime publishes the next
Manifest, conditionally remaps the live Worker Index onto the copies, installs the commit catalog,
retires the old sources, and fails closed whenever restart must complete recovery.
`Store::compact()` schedules a transaction explicitly; an optional Store-owned
[MaintenanceController](maintenance-controller.md) (ADR 0023) may observe and schedule it under
explicit modes and budgets.

## The oldest sealed prefix

Compacting an arbitrary sealed Segment on its own is unsafe. If it discards a tombstone while an
older put remains in another Segment, recovery can resurrect the deleted value. The same issue
applies to expired or superseded puts (TTL removal writes no tombstone). One transaction therefore
selects the **oldest `k` sealed Segments** of one Worker
([ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md)).

Recovery requires a Worker's Segments, in Manifest order, to carry strictly increasing sequence
ranges, so every Record outside the prefix is newer than every Record inside it. A key whose latest
Record lies outside the prefix keeps that winner. A key whose latest Record is a prefix tombstone or
an expired put has every older Record inside the prefix too, so dropping all of them leaves it
absent. A `k` equal to the complete sealed set is the original whole-Worker transaction, which the
v2 blob profile always uses. The active Segment is never rewritten.

The transaction copies only currently visible, non-expired puts whose `RecordRef` points into the
selected prefix. Records are copied in increasing sequence order and keep their v1 sequence, opcode,
value type, flags, key hash, expiration, key and value.

## Identity and Manifest plan

Outputs reuse the earliest source Segment IDs, increment their generations, stay sealed and keep
their Worker owner. Surplus source entries disappear from the next Manifest. Sealed entries after
the prefix, the active entry, `next_segment_id`, routing metadata and every other Worker's entries
stay unchanged.

The transition is therefore determined by `(k, m)`, the prefix length and the output count. The
validator derives both from the exact old and next Manifests, and intent recovery uses the derived
`k` to name the obsolete sources. This preserves global Segment-ID ordering and keeps compacted
sequence ranges before the Worker's active Segment, without changing the Manifest, Segment,
commit-slot or Record v1 formats.

The planner rejects:

- a Worker without a sealed set, a prefix longer than that set, or an output count larger than the
  prefix;
- exhausted Manifest or selected Segment generations;
- plans that reclaim no physical Segment;
- temporary allocation, peak Store size, or physical write amplification outside
  `DurableResourceLimits`;
- a next Manifest that does not pass the ordinary v1 encoder validation.

Physical write amplification is bounded conservatively as preallocated output Segment bytes divided
by Segment bytes reclaimed. Empty output is valid when the selected prefix holds no live Record.

Output sizing uses the exact sequence-ordered Record extents. Aggregate live bytes divided by the
Segment payload is only a lower bound, because Records cannot span Segment boundaries; the builder
uses an allocation-free next-fit layout that also fixes every future `RecordRef` before intent
publication. Peak Store accounting includes the output Segments, both external Manifest files and
both Manifest payloads embedded in the intent.

## Publication and retirement order

The implementation must use this order:

```text
briefly lock target Worker + catalog
  -> copy owning Index entries and pin exact sealed generations
  -> release both locks
  -> select the oldest sealed prefix within the copy budget (ADR 0044)
  -> collect and verify visible prefix Records
  -> create private replacement temporaries
  -> fill, synchronize, seal, and validate every replacement temporary
  -> acquire the publication lease; rebase the identical prefix plan onto the
     current authority when unrelated rotations advanced it
  -> publish and directory-sync an exact compaction intent
  -> rename every replacement temporary to its canonical identity
  -> directory sync once for the complete promoted name batch
  -> try-lock target Worker and lock catalog
  -> reject if sequence, batch, manifest, source identity, or pin changed
  -> prepare the complete non-allocating in-memory publication
  -> mark the target Worker commit-gated and release both locks
  -> atomically publish and directory-sync the next v1 Manifest with no mutex held
  -> re-lock the Worker and conditionally remap every prefix reference to its copy
  -> lock the catalog, merge retained commit metadata, and publish prepared catalog state
  -> clear the commit gate and release both locks
  -> unlink every old source name
  -> directory sync
  -> remove compaction intent
  -> directory sync
  -> final namespace audit
  -> release the old/next publication lease
```

The intent is protocol metadata, not a second data format. It must bind the Store ID, the Worker,
the previous and next Manifest generations and the exact source and replacement identities with a
checksum. It is durable before any unlisted canonical replacement name can appear. Private
`.segment-<id>-<generation>.glifi.tmp` files may exist before the intent, but the authoritative
Manifest never references them, and they are recognized only as disposable engine temporaries.

### Build before the intent

Before acquiring the lease and publishing the intent, the builder verifies the Manifest snapshot,
validates every snapshot Index reference against its catalog identity and routed source Record,
drops expired sealed source puts at the supplied Store time (counted in `expired_records_dropped`),
and records one conditional remap per selected Record: `(key, source RecordRef, output RecordRef)`,
or no output for a dropped expired put. It then creates, copies, seals and checksum-validates every
output under its private temporary name.

Index entries outside the prefix are never touched; expired keys there remain until a lazy GET
reclaim, a later transaction or the next recovery rebuild. Source entries are ordered by their
preserved sequence, and one reusable Record buffer avoids per-read allocation. Encoded v1 Record
bytes are copied exactly into the planned Segment boundaries and checked against exact record-count,
extent, sequence, source CRC and output CRC metadata. The placement vector holds a compact
source-vector index instead of duplicating owning keys and `RecordRef`s. The remap after the future
Manifest commit performs in-place slot updates only, so it neither allocates nor fails.

A pre-intent failure removes the private temporaries and stays `not_started`. A post-intent failure
is explicitly `recovery_required`, and restart rolls the exact temporary and canonical outputs back
under the old authority.

When automatic normal-pressure maintenance configures `max_copy_bytes_per_sec`, each private
replacement Record is emitted through allocation-free byte grants. Grants target 10 ms spacing and
bound every physical write to at most 1 MiB; a Record larger than a grant is split only at the
file-I/O layer and stays one unchanged v1 Record. All waits happen before the publication lease and
the durable intent. Pressure and emergency bypass pacing, and manual `Store::compact()` is unpaced.
The result reports actual pacing wall delay, sleep count and burst bytes separately from the total
pre-intent time.

TTL reclaim has three cooperating paths. Recovery omits expired latest puts from the rebuilt Index.
A validated GET lazily removes Index and hot-cache entries without a durable tombstone. Sealed
compaction drops Index-resident expired puts from the replacement catalog and physically omits them
from the replacement Segments. `Store::compact()` and `MaintenanceSnapshot` expose
`expired_records_dropped`, so operators can measure sealed TTL reclaim independently of the bytes
and Records copied.

### Intent codec and files

The intent codec stores the complete old and next v1 Manifests under a fixed header that holds the
Store ID, the Worker, both generations, the exact payload lengths, the format version, reserved-zero
space and a CRC32C. Decode validates both embedded Manifests and reruns the canonical transition
validator, which derives `k` and `m` from them. It rejects a header and payload disagreement, an
unknown version, trailing data, and any transition that changes routing, active ownership or
next-ID state.

Filesystem publication writes a private exclusive temporary, synchronizes the complete intent,
renames it to the canonical name and synchronizes the directory. A failure known to precede the
rename is `not_published` and cleans the temporary; a rename attempt or a later directory-sync
failure is `indeterminate` and poisons that `DataDirectory`. Reads validate a private, singly linked
regular file, enforce the configured Manifest-derived byte bound before allocating, read the exact
stable extent and decode it. Intent removal distinguishes a fault before `unlinkat` (`not_removed`)
from an unlink attempt or a later directory-sync failure (`indeterminate`).

### Recovery with an intent

On restart, the runtime decodes and validates the intent before ordinary recovery, accepts only an
authoritative Manifest exactly equal to its old or next Manifest, and performs a complete recovery
scan of that authority before deleting anything. Namespace recovery admits only the canonical intent
and the exact non-authoritative Segment identities implied by the validated transition; unrelated,
malformed, linked or missing authoritative entries still fail closed.

Recovery with an intent has only two valid authorities:

- if the old Manifest is authoritative, all old sources must still exist; the exact replacements
  named by the intent are rollback material and may be removed before the intent;
- if the next Manifest is authoritative, every replacement must validate against it; the remaining
  old sources are retirement material and may be removed before the intent;
- any other Manifest generation, identity, missing authoritative file or unrelated namespace entry
  fails closed without adoption.

### Retirement

Retirement opens every still-present obsolete name against its expected immutable identity before
unlinking it. Missing obsolete names are accepted, so recovery can resume after a partial cleanup. A
failure before the first unlink is `not_removed`; any failure after removal begins, including the
mandatory directory sync, is `indeterminate` and poisons that directory instance. A fresh reopen
repeats the same authoritative recovery, finishes the idempotent retirement batch, removes the
intent and runs a final ordinary namespace audit.

No old source is unlinked before the new Manifest and its directory entry are durable, and
`unlinkat` is descriptor-relative. A cold reader holds shared ownership of the exact immutable
generation and its already-open descriptor, so source retirement can remove the pathname after
publication without invalidating that reader; the reader's final locked validation observes the
replacement `RecordRef` and pin and retries before returning. The directory is synchronized after
the retirement batch, so restart observes either names still awaiting cleanup or their durable
removal.

## Concurrency with the Worker

Compaction must not run in a mutation or acknowledgement critical path. The internal
`compact_worker` takes a brief owning snapshot (Phase A), then performs source reads, CRC
validation, replacement writes, sealing and reopen validation without the target Worker or catalog
lock (Phase B). Reads and ordinary mutations continue on that Worker during Phase B and record
nothing for compaction; [ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md) replaced the
ADR 0043 journal.

On the mutex-elided exclusive Writer path, Phase A arms the Worker's `IndexQuiesceGate` and waits
for its Writer depth to drain before enumerating the Index, because the shared catalog lock alone
does not serialize exclusive mutates that skip `worker.mutex`. The gate is disarmed before Phase B,
so ordinary mutations continue during the unlocked build.

A pending or closing group at snapshot time, or a changed source identity, causes a finite
`sequence_conflict` and the durable removal of replacement files and intent under the still
authoritative old Manifest. It causes no fail-close unless the rollback itself becomes uncertain.
Unrelated rotations during the build do not conflict: the intent is rebased onto the current
authority when every source is still the Worker's oldest sealed prefix.

Publication uses a non-waiting Worker try-lock and the catalog-exclusive lock only to validate and
prepare already-reserved vectors. It then arms the Worker-local commit gate and releases every
physical mutex before the quiesce waits and the durable Manifest write, sync, rename and directory
sync. On the mutex-elided exclusive Writer path it waits for the gate's Writer depth to drain (and
otherwise for `!mutation_io_active`) only after that unlock, then reacquires the locks and
revalidates the rebased authority and source pins. A conflict aborts with a finite
`sequence_conflict` and rolls the replacements back under the old authority. Holding the catalog
across the depth wait would deadlock a mutate that already entered the hot path and needs the shared
catalog after its append.

While the gate is armed, reads continue through the unchanged Index and pinned sources, mutations on
that Worker receive a finite `sequence_conflict`, and flush paths wait on a condition variable that
releases the Worker mutex.

After durable publication, with the gate still armed, the runtime locks the Worker and applies the
conditional remap:

- an Index entry still equal to its source reference moves to the copy, and a matching hot-cache
  row follows it;
- an expired drop that is still current is erased and its live bytes subtracted;
- any other entry was superseded during the build and keeps its newer reference, which leaves the
  copy as dead output bytes.

Live byte counters need no other change, because source and copy are both sealed. The runtime then
locks the catalog, refreshes retained commit metadata that other Workers may have advanced, installs
the next catalog and disarms the gate. Source retirement and the final audit run unlocked.

## Publication lease

Persistence v1 admits exactly the old and the next Manifest authority. A Store-wide logical
publication lease preserves that invariant without holding the publication mutex itself through
intent installation, promotion, Manifest I/O or retirement. ADR 0039 keeps source scan, replacement
creation, Record copy, output synchronization, seal and validation outside even the logical lease.
Rotation and a second compaction take the physical serializer only long enough to observe the
lease.

A second compaction returns a finite `sequence_conflict`. Rotation snapshots its Worker sequence,
releases the Worker mutex while waiting on the condition variable, then revalidates both sequence
and authority. If another mutation advanced that Worker, the suspended attempt returns
`not_committed + sequence_conflict`; the daemon may retry the same owned mutation once, while the
embedded API reports the conflict. The lease wait therefore cannot turn into Worker head-of-line
blocking, a third Manifest authority stays forbidden, and persistence v1 is unchanged.

## Scheduling and budgets

The public `Store::compact()` scheduler is cooperative and creates no background thread by itself.
It examines eligible Workers in round-robin order, skips exact layouts that would reclaim no
physical Segment, and completes at most one transaction per call under the configured
temporary-space and amplification limits. A Store-local try-lock rejects a concurrent maintenance
request instead of queuing it. This bounds shutdown: `close()` either sees no compaction or waits
only for the one already admitted transaction to finish.

When `StoreConfig::maintenance.mode` is `background`, a Store-owned
[MaintenanceController](maintenance-controller.md) may run one evaluation thread and invoke
`Store::compact()` under normal-policy budgets. The controller shares the same non-queuing
maintenance gate.

Write-amplification and temporary-space budgets (`DurableResourceLimits`) are enforced at plan time,
before any durable intent (`GS-PERSIST-AMP-001`). `CompactionResult` separates
`pre_intent_duration_ns` from `publication_lease_duration_ns`. It also reports `pacing_delay_ns`,
`pacing_sleep_count`, `pacing_burst_bytes` and `transient_metadata_lower_bound_bytes`; the last
includes source, placement and remap vector capacity and reusable Record scratch. It is not an RSS
claim: copied `std::string` heap storage and allocator bookkeeping require a platform allocation
census.

Telemetry for maintenance latency, reclaim debt and pacing (`GS-OPS-DEBT-001`) is diagnostic
observability; it does **not** retire the ADR 0036 generation-slot-pool publication obligations.

Benefit and cost validation for high, medium and low reclaim, copy-heavy, TTL and no-gain layouts
runs through the compaction and maintenance harnesses under the gitignored `benchmark-results*/`.
Foreground tail latency, controlled cross-platform runs and automatic-policy tuning remain release
work.

## Evidence

The single-output online kill and fault matrix is complete. The process-kill matrix covers 24
boundaries: replacement preallocation and header, the first two Record copies, the batched Record
sync, the write and sync of both commit slots (data boundary and seal), intent write, sync and
rename, replacement rename, Manifest write, sync and rename, both source unlinks, intent removal and
all five directory syncs. Every case reopens through the ordinary recovery entry point and verifies
the rebuilt Index. A complementary pre-operation I/O-fault matrix covers the same 24 boundaries,
checks the runtime outcome and health, and reopens one clean authority that preserves both values.
Allocator interposition enumerates every allocation observed by the online transaction and reopens
after each failure.

Other tests cover tombstone non-resurrection, TTL reclamation, zero-output compaction, generation
and resource limits, and repeated compaction. An online end-to-end case compacts 64 maximum-size
live Records from four sealed sources (the fourth holds only a superseded Record) into exactly two
replacements, verifies every rebuilt reference and reopens every value.

Four independent fixed-seed histories apply 608 PUT, ERASE and expiring-PUT operations in total
across 32 keys, three sealed sources and the active Segment; each sequential model must agree before
compaction, after in-memory installation and after ordinary reopen. A failure reports the exact
reproduction seed. Five more seeds combine another 760 operations with an occurrence-specific I/O
failure before the intent write, during the third Record copy, at Manifest synchronization, during
the second source unlink and before intent removal. Each case verifies the returned outcome, the
healthy or fail-closed state, the selected old or next authority, a clean namespace and the same
model after reopen.

Two-output recovery injects a failure and a SIGKILL after each partial replacement rollback and
partial source retirement position, then proves idempotent cleanup of every remaining identity.
Rollback removes both the canonical and the partially created temporary name of each obsolete
replacement identity, including after a crash while the second output is being created.

The online SIGKILL profiles are:

- single-output: a fixed 30-operation PUT, ERASE and expiring-PUT history across two sealed sources
  and the active Segment; the same eight-key model must survive every one of its 24 persistence
  checkpoints;
- online 3-to-2: 14 checkpoints for the transitions the single-output transaction lacks, namely
  second-replacement preallocation, header and promotion, its batched directory sync, the 64th of 80
  maximum-size Record writes (the first into the second output), its distinct data and seal commits,
  the shifted Manifest, retirement and intent directory syncs, and the third source unlink;
- the opt-in `copy-matrix`: a kill after each of the first 63 Record writes, the writes into the
  first output. With the 89-checkpoint standard matrix it totals 152 distinct checkpoints and covers
  copies 1 to 64 of 80; copies 65 to 80 have no kill point of their own. The source seed batches
  pending Records and makes them durable at seal, so this evidence does not add a sync per seed
  Record;
- `random-matrix`: four reproducible 120-operation histories, each combining PUT, overwrite, ERASE,
  expired PUT and restoration while ending with exactly 88 maximum-size live values, across old- and
  next-authority checkpoints. It SIGKILL-tests nine checkpoint classes per seed: durable intent,
  early and middle copy, second-output creation, the first copy into the second output,
  second-output seal, Manifest authority change, partial source retirement and final cleanup.

A separate bounded `random-campaign` samples a new history seed and checkpoint class per iteration
from one recorded campaign seed, reopens against the same complete sequential oracle and emits a TSV
schedule and outcome report. The hosted weekly profile runs 256 cases; local or dedicated-runner
campaigns may request up to 10,000. This broadens E2 state-space sampling, but it is neither
random-timing coverage nor E3/E4 certification.

Native power-loss campaigns remain certification work: durable compaction cannot be certified across
all supported platforms without native power-loss evidence.
