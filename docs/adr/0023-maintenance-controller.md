# ADR 0023: Optional Store MaintenanceController

- Status: accepted
- Date: 2026-07-20
- Deciders: persistence maintainers
- Applies to: embedded Store and glifistored
- Amends: none
- Supersedes: none
- Related: ADR 0015

## Context

`Store::compact()` is a correct, crash-safe, non-queuing whole-Worker (durable) or selective
vacuum (volatile) primitive. It is cooperative: physical reclaim depends on an external caller.
Without regular compaction, dead bytes and sealed Segment count can grow toward resource limits.
Automatic loops that call `compact()` without budgets risk write amplification and fast-path
interference. The project needs an optional controller that schedules the existing primitive under
explicit modes and budgets without changing ownership, formats, or acknowledgement semantics.

## Decision drivers

- Preserve Worker ownership and the copy-build-validate-publish-retire transaction (ADR 0015).
- Embedded Store must default to no hidden threads.
- Daemon should be able to reclaim space without an external compact caller.
- At most one compaction per Store; never queue concurrent maintenance.
- Policy must be budgeted and observable; Phase 0 must not enable aggressive automatic compact.

## Alternatives considered

- Always-on aggressive background compact: rejected (fast-path and WA risk; hidden work in embedded).
- Parallel per-Worker compaction: rejected for v1 without experimental evidence.
- Rewriting the durable compaction transaction inside a new thread: rejected; reuse `Store::compact()`.
- Compaction only inside mutation paths: rejected (latency coupling; still cooperative for reads-only).

## Decision

1. Add `MaintenanceMode`: `cooperative` (default for embedded), `background`, `disabled`.
2. `glifistored` defaults to `background`; CLI can override.
3. `MaintenanceController` is Store-owned. In `background` it runs one `std::thread` with explicit stop/join
   semantics aligned to `DurableFlushCoordinator`. Cooperative and disabled start no thread.
4. The controller never mutates Worker Index or Segments directly. Future policy may only observe
   published snapshots/stats and invoke `Store::compact()`, which already serializes on
   `compaction_mutex`.
5. Phase 3 enables automatic `Store::compact()` under **normal**, **pressure**, and **emergency**
   policies when mode is `background`. Under emergency, `put`/`erase` are rejected with
   `ErrorCode::storage_exhausted` while reads, flush, compact, and close continue.
6. Concurrent manual `compact()` and controller-driven compact share the same try-lock; conflicts
   return `sequence_conflict` without queuing.
7. `Store::close()` stops maintenance wake, waits for admitted operations (including in-flight
   compact), joins the controller, then proceeds with existing flush/runtime teardown.
8. Policies must not introduce unbounded queues; emergency never buffers rejected mutations.
9. Normal durable scheduling uses exact published per-Worker Index-referenced sealed/live/dead
   Record-byte counters. It skips below `dead_byte_ratio_bp_normal`, while pressure/emergency bypass
   the threshold and pass the observed Worker identity to the existing compact transaction.
10. One normal evaluation preflights that candidate's exact live Record bytes against the inclusive
    `max_copy_bytes_per_cycle` limit. The default is 128 MiB; zero explicitly means unlimited.
    Pressure/emergency bypass the limit because reclaim is then capacity-preserving work. The
    durable transaction rechecks the limit at its locked snapshot boundary; concurrent growth
    fails with `sequence_conflict` before scanning or copying.
11. An exact no-gain result is memoized only for the same physical candidate (Worker identity,
    sealed count, sealed/live/dead bytes, scheduling ratio, and unread-expiry counters). After
    `max_no_gain_attempts` exact results (one by default), normal maintenance suppresses another
    whole-candidate scan until the candidate changes or `max_eval_interval_ms` elapses. Pressure
    and emergency bypass this suppression. The time bound preserves eventual re-evaluation for TTL
    progression even when no physical counter changes. `max_segments_per_cycle` remains only a
    source-compatibility guard and must equal one: persistence v1 still admits exactly one complete
    Worker sealed-set transaction per call.

## Consequences

Positive: clear lifecycle and config surface; daemon path ready for autosufficient reclaim; no format
change; compact remains the ground-truth transaction; emergency fails closed on capacity.

Negative / deferred: unread TTL stays Index-live under default normal maintenance until GET,
recovery, pressure, or an explicit `Store::compact()` visit. Opt-in
`unread_ttl_normal_scheduling` adds unread expired sealed puts to the normal dead-byte threshold
via the same probe and `Store::compact()` path. Per-second and CPU maintenance rate limits are
enforced under normal policy (one-second window; pressure/emergency bypass). Background mode adds
one Store thread when enabled. Durable compaction crash/I/O matrices and native power-loss
certification remain under ADR 0015.

Repeated no-gain work is now bounded: the controller does not repeatedly pay the full source scan
for an unchanged deterministic layout. This does not make a useful v1 compaction preemptible; its
copy/publish transaction remains monolithic and is still a measured residual risk.

## Compatibility and migration

- New optional `StoreConfig::maintenance` fields; defaults preserve cooperative embedded behavior.
- No disk, wire, or Manifest format change.
- Public API gains maintenance snapshot diagnostics; `Store::compact()` semantics unchanged.

## Verification

- Unit tests: cooperative starts no thread; background starts and joins on close; invalid intervals
  rejected; the normal threshold is inclusive; pressure bypasses it and selects the observed
  Worker; the normal copy limit rejects one byte over, accepts equality, and is bypassed under
  pressure; close with background mode is clean; pressure continues under no-gain budget; emergency
  rejects put/erase with `storage_exhausted` and recovers when watermarks clear; emergency gate
  survives compact fault; unchanged no-gain candidates suppress repeated scans; candidate change,
  bounded expiry, and pressure re-admit the attempt; close/flush under emergency remain correct.
- Integration: durable catalog-driven emergency; Store close during blocked background compact drains
  then joins; recovery, rotation, overwrite, and reopen preserve candidate byte counters; manual
  `compact()` still works; concurrent compact still returns `sequence_conflict`.
- Remaining before claiming production automatic reclaim efficacy: production benches (not lifecycle).

Phase 5: emergency reclaim faults must not latch auto-compact off while the mutation gate is armed;
background start evaluates immediately; free-space emergency matches rotate headroom.
Phase 6: wire `OVERLOADED` portable retryability is `never`; durable lanes reject before enqueue when
the emergency gate is armed.

## References

- [ADR 0015](0015-whole-worker-compaction.md)
- [Crash-safe durable compaction](../architecture/durable-compaction.md)
- [Maintenance controller](../architecture/maintenance-controller.md)
- [C++ API reference](../reference/cpp-api.md)
