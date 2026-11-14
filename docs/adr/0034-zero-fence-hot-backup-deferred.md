# ADR 0034: Zero-fence hot backup — required design before implementation (deferred)

- Status: accepted
- Date: 2026-08-01
- Deciders: persistence / platform maintainers
- Applies to: durable catalog copy while writers continue without admission fencing; not
  implemented in 0.1.x
- Amends: none (extends the non-goal in [backup-restore v1](../spec/backup-restore-v1.md))
- Supersedes: none
- Depends on: [0003](0003-log-indexed-storage.md), [0004](0004-index-as-derived-state.md),
  [0008](0008-alpha-persistence-contract.md), [0015](0015-whole-worker-compaction.md),
  [0032](0032-paired-concurrency-embedded-store.md)

## Context

GlifiStore already supports:

1. **Offline** verified catalog copy (`glifistore_backup_store`) with exclusive data-dir lock.
2. **Online fenced** copy (`Store::backup_to` / wire `BACKUP`): fence new admissions, drain
   in-flight work, flush, copy under catalog lock, resume. Concurrent public ops may observe
   `unavailable` during the fence.

Operators and the production roadmap still list **fully concurrent hot backup** (writers continue;
no admission fence) as desirable. Implementing that by “copy files while PUT runs” without a frozen
consistency model risks torn Manifest/Segment sets, backup of uncommitted tails, or backups that
pass byte copy but fail recovery. This ADR freezes the **design bar** for any zero-fence path and
defers implementation past 0.1.x.

## Decision drivers

- Backup destination must remain a recovery-consistent catalog (same boundary as backup-restore v1).
- Writers must not observe a longer unavailability window than today’s fenced path claims to remove.
- Crash during backup must not corrupt the source Store.
- Compaction/rotation concurrent with backup must not yield a Manifest that names missing or
  half-copied Segments in the destination.
- Prefer explicit mechanisms (COW, frozen generation, filesystem snapshot) over best-effort rsync.

## Alternatives considered

1. **Ship “copy while open” without fencing as 0.1.x.** Rejected: no consistency proof.
2. **Filesystem freeze / LVM / ZFS / APFS snapshot orchestration inside GlifiStore.** Deferred as
   an optional operator integration: valuable, but platform-specific and outside the durable
   catalog contract; may be recommended as an *external* zero-downtime approach without claiming a
   GlifiStore API.
3. **Extend the fenced path only (shorter fence, copy parallelism).** Accepted as incremental
   improvement to the **existing** online path; does **not** satisfy zero-fence claims.
   Status: online `Store::backup_to` resumes admissions after catalog copy and runs destination
   verify outside the fence; catalog Segment copies use bounded parallelism (Manifest last);
   source validation under the fence is **structural only** (destination CRC is the promotion
   gate); reports `admission_fence_ns` / `catalog_copy_ns` / `destination_verify_ns` /
   `segment_copy_workers`. Concurrent callers retain counted admission-fence ownership across the
   catalog-copy serialization wait. Still fenced for flush+structural check+copy.
4. **Multi-version Segment freeze (pinned Manifest generation + copy-on-write new writers).**  
   Leading candidate for an in-process zero-fence design; requires durable generation pins and
   interaction with compaction (ADR 0015) and paired Writers (ADR 0032).
5. **Dual-directory mirror with async ship.** Rejected as default backup API: different product
   (replication), not a point-in-time catalog snapshot.

## Decision

### A. 0.1.x behavior (unchanged)

1. Offline and online **fenced** backup remain the only supported product paths.
2. Wire `BACKUP` and SDK `backup` helpers keep fenced semantics.
3. Documentation must not describe fenced backup as zero-impact or zero-fence.

### B. Required design for any future zero-fence hot backup (not implemented)

A release that claims zero-fence hot backup **must** specify and prove all of the following:

1. **Frozen catalog generation**  
   A durable or crash-recoverable identifier for the Manifest + Segment set being copied, such that
   concurrent writers either (a) do not mutate those bytes, or (b) mutate only COW replacements
   that the backup explicitly does not need.

2. **Exclusion of uncommitted tails**  
   Active Segment regions not named by the frozen Manifest must not appear as authoritative in the
   destination. Uncommitted extents stay invisible per persistence v1 recovery rules.

3. **Compaction / rotation exclusion or coordination**  
   Either compaction/rotation is deferred for the frozen generation, or the backup algorithm
   includes those operations in its linearization and fault matrix (retire-after-publish rules).

4. **Admission behavior**  
   Define whether GET/PUT never see `unavailable` from backup, or only outside a documented
   micro-critical section shorter than today’s fence (and measure it). “Zero-fence” means no
   Store-wide admission close comparable to `backup_to` today.

5. **Destination verify**  
   Same post-copy `verify_durable_store` requirement as backup-restore v1. Fail closed on
   incomplete copy.

6. **Fault matrix**  
   Process-kill and I/O faults during copy: source remains serving; destination is never promoted
   without verify; no dual Manifest authority on source.

7. **API honesty**  
   New API/flag/opcode (or major wire revision) distinct from today’s fenced `BACKUP`, or an
   explicit capability bit—so clients do not assume zero-fence from opcode 10 alone.

### C. Recommended interim operator approach

For zero-downtime needs before an in-process design lands: take a **volume/filesystem snapshot**
of a quiescent or crash-consistent volume, then run offline verify/copy from the snapshot clone.
That is an operator procedure, not a GlifiStore catalog feature, and must be labeled as such.

## Consequences

**Positive:** Prevents marketing fenced backup as hot; gives a clear engineering bar for a future
project; preserves recovery invariants.

**Negative:** Under load, online backup still briefly fences admissions; some HA designs need
external snapshots.

**Deferred work:** Generation-pinned COW backup, optional FS snapshot helpers, benchmarks of fence
duration vs copy size on labeled hardware.

## Compatibility and migration

- No change to Manifest, wire opcode 10 semantics, or offline CLI in this ADR.
- A future zero-fence feature needs fixtures, N↔N-1 notes if on-disk generation metadata is added,
  and client/SDK documentation separate from fenced `backup`.

## Verification

For this ADR (design-only): doc review; links from backup-restore v1, architecture backup-restore,
operations runbook, and final engineering report residual.

For a future implementation: crash/I/O fault matrix under concurrent PUT/compaction, destination
verify always, fence-duration metrics only if a reduced-fence claim is made, and hardware-backed
copy throughput evidence—not satisfied here.

## References

- [Backup and restore v1](../spec/backup-restore-v1.md)
- [Operations backup-restore](../operations/backup-restore.md)
- [ADR 0015 whole-Worker compaction](0015-whole-worker-compaction.md)
- [ADR 0032 paired embedded concurrency](0032-paired-concurrency-embedded-store.md)
