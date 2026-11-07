# ADR 0043: Bounded concurrent delta for whole-Worker compaction

- Status: accepted
- Date: 2026-11-07
- Deciders: storage and concurrency maintainers
- Applies to: paired runtime durable compaction; persistence v1; wire v2
- Amends: ADR 0015, ADR 0039
- Supersedes: none

## Context

ADR 0039 stages verified replacements before the global publication lease, allowing foreground
mutations during most I/O. Its sequence equality check nevertheless rejects the entire prepared
transaction after a selected-Worker mutation. The initial owning snapshot also enumerates the
complete mutable Index under synchronization; immutable paired Reader generations do not remove
that cost. Reader base/delta merging and exact file-generation pins already exist and remain distinct
from durable compaction publication.

## Decision

Keep whole-Worker sealed-history replacement, the active Segment, exact `Mold/Mnext` intent,
Manifest authority, write/sync/rename ordering, acknowledgements and client error mappings unchanged.
Replace the no-mutation validation with an owning, bounded journal of Index publications:

1. Preallocate journal entry slots and a key-byte arena before entering snapshot synchronization.
   Attach it to the selected Worker after capturing the complete Index and source pins. Concurrent
   low-level compaction cannot attach a second journal to that Worker.
2. Every successful Index PUT/ERASE publication, including strict group publication, appends its key,
   hash and resulting optional RecordRef. Physical appends awaiting group publication are not logged
   as visible. TTL Index removals are logged too. The publisher already owns the mutable Index;
   recording adds no lock, allocation, value copy or fallible foreground admission step.
3. The journal has independent entry and key-byte limits: defaults 4096 events / 1 MiB, hard ceilings
   65536 events / 64 MiB. Repeated changes count separately. Zero allows no changes in that dimension.
   Exhaustion sets a sticky local overflow flag and stops logging. Foreground mutations continue.
4. Before intent publication, reserve the replacement Index for the worst-case replay entry/key
   growth. Reservation failure discards private staging under `Mold`. The complete Manifest must
   still equal the captured snapshot: any rotation, including an unrelated Worker's, rejects staging.
5. After staging/promotion, retain the existing Worker commit gate and drain its exclusive hot-path
   depth and mutation I/O without holding Worker/catalog mutexes. Reject pending/closing groups,
   unhealthy state, changed Manifest/source pins or journal overflow using existing rollback rules.
6. Freeze TTL Index housekeeping and detach the journal while holding Worker/catalog synchronization.
   Release both mutexes. Replay the bounded journal into the private replacement Index in publication
   order, using `erase_no_compact` and reserved insertion capacity. Post-cut PUT references must still
   name the unchanged active Segment. Recompute active/sealed live byte counts from actual replayed
   references, independently of physical copy bytes. Expired GETs still miss while housekeeping is
   frozen; no expired value is made visible by deferral.
7. Publish/sync `Mnext`, then install Index/catalog state and unfreeze the Worker. Continue source
   retirement, intent removal and final audit under the original v1 publication lease. Existing pins
   retain old open descriptors until their readers finish; pathname removal is not descriptor reclaim.

No sequence allocation, durability watermark, Reader visibility or acknowledgement point changes.
The delta is private metadata, never recovery authority. Replay failure before `Mnext` invokes the
existing exact rollback. Ambiguous persistent failures still fail closed and require ordinary reopen.

## Alternatives

- Reject every sequence change: safe fallback when the journal overflows, but wastes staging work.
- Rebuild the full mutable Index at final publication: shifts O(N) enumeration/allocation into the
  publication pause, instead of replaying an explicitly bounded number of changes.
- Rebase across rotations: deferred. It requires rebuilding the exact old/next plan and validating
  recovery composition; an in-memory CAS does not authorize a third v1 Manifest authority.
- Partial sealed sets / persistent MVCC / asynchronous cleanup outside the lease: deferred to a
  separate accepted contract. They change tombstone closure or namespace recovery obligations.

## Observability and acceptance

Compaction results expose snapshot, commit-gate and replay durations, journal events/key bytes,
overflow/Manifest aborts and abandoned copy bytes. Snapshot duration includes gate/lock waiting and
Index enumeration. Commit-gate duration includes drain/replay and durable Manifest publication;
the global lease additionally includes promotion and cleanup. Runtime cumulative counters are
available through `Store::maintenance_snapshot().compaction`, including failed attempts.

The maintenance benchmark reports operation/GET/PUT p99.9 plus journal, abort, wasted-copy and
maximum snapshot/gate telemetry alongside rotation waits. A local smoke sample is instrumentation
evidence, not proof of a p99.9 improvement. Sustained same-Worker writes can exhaust the journal;
bounded abort protects foreground work but does not guarantee reclaim progress at arbitrary load.
Initial O(N) snapshot cost and global v1 cleanup exclusion remain measurable residuals.

Required controls include same-key PUT/ERASE/PUT, active and newly inserted keys, TTL removals,
sync/exclusive/group/periodic modes, entry/key/zero limits, successful retry after overflow,
unrelated rotation, existing Reader pins, exact live counters, I/O faults, SIGKILL across publication
and retirement, allocation faults and sanitizers. Native macOS/Linux/FreeBSD/OpenBSD campaigns are
platform-specific; local proof does not promote hosted gates or certify E3/E4.

## Compatibility

All persistent codecs remain v1 and wire remains v2. Diagnostic C++ fields and resource limits are
source-level additions; the stable C ABI is unchanged. This supplies no transactional snapshot API.
External 64 MiB blobs remain the separate proposed ADR 0042: metadata-only blob compaction needs
its own descriptor roots, value separation and GC proofs.

## References

- [Whole-Worker compaction](0015-whole-worker-compaction.md)
- [Pre-intent staging](0039-pre-intent-staged-compaction-output.md)
- [Write pacing](0040-pre-intent-compaction-write-pacing.md)
- [Durable compaction](../architecture/durable-compaction.md)
- [Recovery state matrix](../spec/recovery-state-matrix-v1.md)
- [Large blob proposal](0042-large-blob-storage.md)
