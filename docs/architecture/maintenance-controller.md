# Maintenance controller

**Descriptive** · embedded Store and `glifistored` · reviewed 2026-09-15

Decisions: [ADR 0023](../adr/0023-maintenance-controller.md) for the controller and
[ADR 0019](../adr/0019-client-error-retry-timeout.md) for the wire retry mapping.

How the optional `MaintenanceController` observes capacity, schedules and paces compaction, and
rejects mutations in an emergency. It does not replace `Store::compact()`: the durable
oldest-sealed-prefix transaction and the volatile selective vacuum remain the only compaction
primitives ([ADR 0015](../adr/0015-whole-worker-compaction.md), amended by
[ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md)).

## Modes

| Mode | Embedded default | Daemon default | Thread |
| --- | --- | --- | --- |
| `cooperative` | yes | no | none; caller invokes `compact()` |
| `background` | no | yes | one Store-owned `std::thread` with explicit stop/join |
| `disabled` | no | no | none; `compact()` still available |

## Evaluation

An evaluation observes catalog-level statistics (`MaintenanceObservation`) without scanning Worker
Indexes or reading Segment Records. Recovery, already-serialized Index publication, lazy expiry,
rotation and compaction maintain exact active and sealed Index-referenced byte counters. The
observation selects the next round-robin Worker with sealed history and reports its sealed, live,
dead and dead-ratio counters. Observation still runs when auto-compact is disabled, so emergency
admission stays fail-closed. In `background` mode, start requests an immediate first evaluation,
with no mid-interval blind window.

`classify_maintenance_pressure` then assigns the pressure level; the highest severity wins.

| Level | Condition |
| --- | --- |
| emergency | `segment_count >= max_segment_count`, or available free bytes cannot cover `reserved_free_bytes + rotate_additional_bytes`, so Segment creation or rotation is at risk |
| segment pressure | `segment_count >= ceil(max_segment_count * segment_count_pressure_pct / 100)` |
| free-space pressure | `available_free_bytes <= reserved_free_bytes + free_bytes_pressure_margin`, when emergency does not already apply |

The catalog observation sets `rotate_additional_bytes` to one Segment plus the next Manifest size,
matching the `rotate_active` headroom. With the default margin of `0`, free-space pressure is
reachable only when the margin is raised above that rotate headroom.

Under pressure or emergency, the controller evaluates at `min_eval_interval_ms`, keeps attempting
compaction despite the no-gain memo and the copy budget, and records the activation reason:
`segment_pressure`, `free_space_pressure` or `emergency_capacity`.

## Emergency mutation gate

In an emergency the controller publishes `mutations_rejected`. `Store::put` and `Store::erase` then
return `ErrorCode::storage_exhausted` with a stable message, as do the exclusive `StoreAccess`
paths: paired synchronous `put/erase_volatile_published` with `caller_holds_guard`, each sibling in
`mutate_durable_batch`, and each item in non-paired `Store::put_batch` and `Store::erase_batch`.
`get`, `flush`, `compact` and `close` continue. There is no mutation queue.

### Mid-batch contract

The gate applies to every sibling and batch item, not only at batch entry. The synchronous Writer
fast path skips only the nested `OperationGuard` read-modify-write, never this gate, so a gate armed
in the middle of a batch (TOCTOU) still rejects the later mutations before Store append.

### Reclaim faults

The gate fails closed on a reclaim fault. A compaction or observation fault keeps an
already-published emergency gate, and while the gate is armed auto-compact is not latched off:
reclaim keeps retrying under budget. A non-emergency fault still disables auto-compact. `faulted`
stays set for telemetry until a later evaluation recovers or `request_stop` runs.

The gate clears only on a later non-emergency observation, on `unavailable` or `store_closed`, or on
`request_stop` or `join`.

### Wire mapping

The Reactor maps `storage_exhausted` to `ResponseStatus::overloaded`, the existing many-to-one
collapse with admission limits. Official clients advertise `retryability=never` for `overloaded`,
because the wire cannot distinguish capacity exhaustion from queue pressure. An application that
still wants backoff-retry for true admission overload must opt in explicitly.

## Normal policy

A normal evaluation runs at the midpoint between `min_eval_interval_ms` and `max_eval_interval_ms`
and performs at most one compaction. It skips, and reports the reason, when:

- there is no candidate (`no_candidate`);
- the candidate's dead ratio is below the inclusive `dead_byte_ratio_bp_normal` threshold
  (`reclaim_threshold`);
- the no-gain memo holds the candidate (`budget_backoff`);
- the rate window is already consumed (`rate_budget`).

The automatic compact call receives the selected Worker identity and the copy budget, so policy and
execution address the same round-robin candidate under the same limit.

`max_copy_bytes_per_cycle` is 128 MiB per evaluation by default. Equality is allowed, and zero
explicitly disables the limit. Since [ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md) the
controller no longer skips a Worker whose live bytes exceed the budget, and it does not report
`copy_budget`. The durable runtime selects, within the budget, the oldest sealed prefix that
reclaims the most Segments. It returns `not_beneficial` before any Record read when even the oldest
sealed Segment exceeds the budget. The rate-window check compares the budget-bounded copy, not the
Worker's total live bytes.

The no-gain memo holds an unchanged candidate whose exact planning found no gain after
`max_no_gain_attempts` attempts (default one). A changed Worker or candidate footprint invalidates
it immediately, and it expires after `max_eval_interval_ms`. It bounds repeated whole-candidate
scans while keeping a time-based retry for TTL progression.

### Latency guard

When `suspend_on_p99_latency_ms` is nonzero, the daemon also consumes a fixed lock-free histogram of
durable mutation admission-to-completion latency and defers an otherwise eligible compaction when
the window p99 reaches that inclusive threshold. The threshold defaults to zero (disabled).

- A decision requires `suspend_on_p99_min_samples` samples (default 32); a smaller window is treated
  as a low-traffic reclaim opportunity.
- Once armed, the guard resumes below 80% of the threshold, which prevents oscillation at the
  boundary.
- `max_latency_deferral_ms` admits one normal reclaim attempt after continuous deferral (default
  30 s); zero defers until pressure.
- Latency feedback is consumed before a normal compaction starts. Pressure and emergency bypass it,
  so reclamation cannot starve when capacity is at risk.

## Rate and CPU budgets

Under normal pressure, `max_copy_bytes_per_sec` paces private pre-intent replacement writes: one
bounded initial burst, 10 ms refill spacing and physical writes no larger than 1 MiB. A candidate
larger than one second of bandwidth is admitted and progresses; the rate is never reinterpreted as
a whole-transaction cap. Manual `Store::compact()` is unpaced.

Completed bytes and `max_cpu_ms_per_window` share a one-second `steady_clock` controller window, so
an adjacent compaction cannot immediately take another initial burst. When the window is already
consumed, evaluation skips with `rate_budget` until it refreshes. Zero disables each control;
pressure and emergency bypass both, as they bypass the per-cycle copy limit.

Daemon flags: `--maintenance-max-copy-bytes-per-sec`, `--maintenance-max-cpu-ms-per-window`,
`--maintenance-suspend-on-p99-latency-ms`, `--maintenance-suspend-on-p99-min-samples` and
`--maintenance-max-latency-deferral-ms`.

## Live bytes and unread TTL entries

The live-byte counter means currently Index-referenced, not guaranteed unexpired at observation
time. Expiry discovered by a validated GET updates it immediately; a cold, unread TTL entry stays
conservatively live until a GET, recovery or compaction visits its Record.

Under pressure or emergency, an optional bounded probe (`unread_ttl_pressure_probe`, default on)
reads sealed source Records of the round-robin candidate only. It exports
`candidate_unread_expired_sealed_record_{count,bytes}` and `unread_ttl_probe_performed` in
`MaintenanceObservation` and daemon `STATS`. On the mutex-elided exclusive Writer path, the probe
releases the observation catalog lock, then arms `compaction_commit_active` and waits for
`hot_path_depth == 0` before enumerating the Index, the same ownership protocol as compaction
Phase A; it never holds the catalog across the depth wait.

Normal scheduling stays conservative by default (`unread_ttl_normal_scheduling`, default off):
unread expired sealed puts remain Index-live for the inclusive dead-byte threshold until a GET,
recovery, pressure or an explicit `Store::compact()` visits them. When it is enabled, normal
evaluations also probe and add unread expired bytes to `candidate_scheduling_dead_byte_ratio_bp`,
for threshold decisions only. The copy budget still uses exact Index-referenced live bytes, and
compaction still goes through `Store::compact()` alone.

## Telemetry

`MaintenanceSnapshot` reports:

- pressure level, `mutations_rejected` and activation reason;
- evaluation and compaction durations, bytes and Records copied, and `expired_records_dropped`
  (last and total);
- suspend count and time since the last useful compaction;
- candidate Worker, its sealed, live and dead Record bytes, and its dead ratio in basis points;
- `no_gain_scans_suppressed` and `no_gain_retry_after_ns`: the work the no-gain memo avoided and its
  bounded remaining lifetime;
- for exact no-gain planning scans, last and total `source_records_verified`,
  `source_bytes_verified` and `expired_records_dropped` examined before the layout rejected a
  rewrite; cheap policy skips (`reclaim_threshold`, `no_candidate`) do not update these fields;
- `MaintenanceSnapshot::sequence_conflicts`, the compaction attempts rejected by a concurrent state
  change;
- rate-window consumption.

Durable snapshots add rotation attempts and commits, compaction waits, final-Record attempts and
commits, and last, total and maximum durations for publication wait, Segment seal, replacement
Segment creation, Manifest publication, aggregate execution, complete rotation and the
post-rotation final Record commit.

Daemon `STATS` exports the candidate, no-gain planning, skip-reason and rotation counters;
`maintenance_sequence_conflicts`; rate-window consumption as `maintenance_rate_window_bytes_copied`
and `maintenance_rate_window_cpu_ns`; and the consumed sample count, bucket-conservative p99, guard
state and age, cumulative suspension count, debt overrides, last and total pacing delay, pacing
sleep count and physical burst.

## Concurrency

Manual and automatic compaction share `compaction_mutex` through `try_to_lock`, so at most one
`Store::compact()` is in flight. A busy gate returns `sequence_conflict`; there is no compaction
queue.

The automatic candidate cursor advances when a Worker is observed, not only when compaction starts.
Threshold and other policy skips therefore cannot pin evaluation to one Worker and starve
reclaimable peers (HAZ-026). Adversarial proofs: the unit test
`reclaim_threshold skip advances to a reclaimable peer Worker` and the integration test
`background reclaim_threshold skip advances past live-only Worker to reclaimable peer`.

## Shutdown

`Store::close()`:

1. stops admission of new Store operations;
2. calls `MaintenanceController::request_stop()`, which clears callbacks and the mutation gate and
   wakes the thread;
3. requests a durable flush and waits until no admitted operation remains, including an in-flight
   `compact()`;
4. calls `join()` on the maintenance thread;
5. closes the durable runtime and releases Store resources.

## Deferred work

- Controlled-hardware baselines and native power-loss certification. The ADR 0015 compaction
  transaction owns them, not this scheduler.
- Mid-transaction pause and resume after durable intent publication.
  [ADR 0040](../adr/0040-pre-intent-compaction-write-pacing.md) paces bounded physical writes only
  while replacements are private and the old Manifest remains the sole authority; the persistence v1
  intent and Manifest transaction remains deliberately non-preemptible.
- Shorter compaction publication leases. Deep rotation phases must be measured first; only then can
  it be decided whether replacement Segment construction should move before publication authority.

`max_segments_per_cycle` is not a partial-transaction control: it is kept for source compatibility,
and validation accepts only `1`. One transaction replaces the oldest sealed prefix of one Worker,
whose length the runtime selects within `max_copy_bytes_per_cycle`
([ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md)). ADR 0040 may split private replacement
Record writes into bounded physical extents; it does not split the logical compaction authority or
weaken tombstone or Manifest semantics.
