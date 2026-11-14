# ADR 0040: Pre-intent compaction write pacing

- Status: accepted
- Date: 2026-08-27
- Deciders: persistence and performance maintainers
- Applies to: normal-pressure durable background compaction, persistence format v1
- Amends: ADR 0023, ADR 0039
- Supersedes: none

## Context

The original `max_copy_bytes_per_sec` controller budget was a completion-accounting gate around a
whole-Worker transaction. It could defer a second transaction, but it did not regulate I/O inside
the admitted copy. Worse, treating the remaining bytes in a one-second window as a transaction
size limit permanently starved every valid candidate larger than the configured rate.

ADR 0039 moved replacement construction under private temporary names before the durable intent.
That creates a safe interval in which maintenance may wait: the old Manifest is still the sole
authority, no replacement is visible, and failure can discard the private output without recovery.

## Decision drivers

- Bound foreground-visible compaction write bursts and reduce tail latency under mixed load.
- Preserve whole-Worker tombstone semantics and persistence-v1 byte compatibility.
- Never delay inside the recovery-sensitive intent/Manifest publication window.
- Guarantee progress for candidates larger than one second of configured bandwidth.
- Keep pacing allocation-free, single-owner, observable, and bypassable under capacity pressure.

## Alternatives considered

- **Whole-transaction admission cap:** rejected because it is not an I/O rate limiter and can starve
  large candidates forever.
- **Sleep after intent publication:** rejected because it lengthens ambiguous recovery authority.
- **One `pwrite` per complete Record:** rejected because a client value larger than the burst would
  still create an unbounded physical write despite correct average timing.
- **Global device coordinator:** potentially useful for several Stores sharing one device, but not
  justified in the current single-Store prototype and would add a shared synchronization domain.
- **Platform I/O priority only:** useful as an optional complement, but not portable across macOS,
  Linux and BSD and insufficient as the sole quantitative bound.

## Decision

Normal background compaction applies an allocation-free, monotonic byte schedule while writing
private staged replacements. The schedule permits one initial burst and then spaces grants by the
configured byte rate. Its target refill interval is 10 ms; a grant is at least one byte and at most
1 MiB. `DurableSegmentFile` splits the Record extent into exactly those granted physical writes, so
the configured burst bounds the actual `pwrite` request rather than only delaying a later large
write. The Record remains logically appended only after every chunk succeeds.

Pacing occurs only in this interval:

```text
verified immutable source + private staged output
  -> acquire bounded byte grant
  -> write one bounded output extent
  -> repeat until Record complete
  -> seal, sync and verify all private output
  -> stop pacing
  -> acquire publication lease
  -> publish intent -> promote -> publish Manifest -> retire -> remove intent
```

The Store controller no longer passes the current one-second remainder as a whole-transaction copy
cap. A fresh candidate is admitted even when its live bytes exceed one second of bandwidth. After a
job consumes the controller window, another candidate waits for refresh; this prevents adjacent
jobs from each taking a new initial burst. The independent `max_copy_bytes_per_cycle` safety cap is
unchanged.

Pressure and emergency bypass both pacing and the controller rate window so reclaim cannot deadlock
on capacity. Manual `Store::compact()` remains unpaced. Zero remains the generic API default.
Named daemon profiles provide measured starting points, not universal device claims: `embedded`
uses 64 MiB/s and `production` 128 MiB/s; file, environment and CLI settings override them.

## Consequences

Foreground I/O receives breathing room during normal reclaim, at the cost of a longer private
staging phase and slower debt convergence. The scheduler and filesystem may oversleep; measured
wall delay is therefore exported separately from nominal rate. Large Records use more syscalls but
do not change their v1 bytes, append point, commit slots, checksums, sequence, or visibility.

There is no device-wide fairness guarantee when multiple Store processes share a volume. Operators
must tune the rate on the real filesystem and storage device, comparing foreground p99/p99.9,
maintenance completion time, reclaim debt and CPU/syscall cost. Pressure bypass can still create an
unpaced burst and is intentionally observable as a capacity-recovery condition.

## Compatibility and migration

Persistence v1, wire v2, routing, acknowledgement, visibility, recovery and tombstone behavior are
unchanged. The profile defaults affect only named daemon profiles and are overrideable. New public
C++ result/snapshot fields are diagnostic additions; the stable C ABI is unchanged.

## Verification

- Deterministic unit tests cover disabled, tiny-rate, rounding, overflow and burst scheduling.
- Segment tests observe that every physical Record write is no larger than the granted burst.
- Durable compaction tests prove nonzero pacing delay/count, successful publish and clean reopen.
- Controller tests prove a candidate larger than one second of rate is admitted and pressure
  bypasses the bound.
- Daemon configuration tests prove profile defaults and explicit override precedence.
- Wave 3: paced write faults during private staging remain pre-intent without residue
  (`GS-OPS-DEBT-001` observability; persistence v1 authority unchanged).
- The maintenance A/B retains disabled, unlimited-background, 64 MiB/s and 128 MiB/s samples with
  foreground throughput, p99, maximum latency and pacing telemetry.
- ASan, TSan, fault injection, crash recovery and assurance validation remain required.

## References

- [ADR 0023](0023-maintenance-controller.md)
- [ADR 0039](0039-pre-intent-staged-compaction-output.md)
- [Durable compaction](../architecture/durable-compaction.md)
- [Maintenance controller](../architecture/maintenance-controller.md)
- [Benchmark standard](../spec/benchmark-standard.md)
