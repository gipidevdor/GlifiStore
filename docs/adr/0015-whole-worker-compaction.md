# ADR 0015: Whole-Worker sealed-history compaction

- Status: amended
- Date: 2026-07-19
- Owners: persistence maintainers
- Related: ADR 0003, ADR 0004, ADR 0008
- Amended by: ADR 0039, ADR 0043

## Context

Compacting one sealed Segment in isolation can discard a tombstone while retaining the older value
in another Segment, resurrecting deleted or expired state. Durable replacement must also survive a
crash between output creation, manifest publication, and source retirement.

## Alternatives considered

- rewrite active Segment in place: rejected because it destroys append-only authority;
- compact one arbitrary Segment: rejected because cross-Segment history may be required;
- retain every tombstone forever: correct but prevents effective reclamation;
- stop-the-world whole-Store compaction: rejected because unrelated Workers can remain independent.

## Decision

One transaction selects the complete sealed history of one Worker and never rewrites its active
Segment. It copies only currently visible, non-expired puts, preserving sequence and Record metadata,
then uses a checksummed intent containing exact old and replacement manifests. Replacement files are
validated before manifest publication; old sources are retired only after the replacement namespace
is durable. `Store::compact()` runs at most one explicit transaction per call.

## Consequences

Tombstones and obsolete values can be dropped safely because every older sealed source for that
Worker retires together. Other Workers continue. Temporary space and physical write amplification
must be preflighted. ADR 0039 moves complete replacement copy, seal and verification before the
durable intent and global publication lease. ADR 0043 journals bounded concurrent Index changes and
replays them during final quiescence; overflow or changed Manifest/source identity still aborts.

## Compatibility and verification

Record, Segment, and Manifest remain format v1; compaction intent has its own v1 codec. Recovery must
resolve old or replacement authority deterministically after interruption. The complete algorithm is
specified in [Crash-safe durable compaction](../architecture/durable-compaction.md).

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
V2 permits descriptor-history compaction without a Segment-count gain when verified obsolete blob bytes satisfy the existing write-amplification budget. The v2 active log rotates before a blob PUT once it contains 32 Records; legacy v1 policy is unchanged.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
