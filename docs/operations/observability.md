# Observability

**Descriptive** · `glifistored` probes, `STATS`, JSON logs, `--dump-config` · reviewed 2026-08-27

The operator signals `glifistored` implements, their units and cardinality, and how to use them. The
probe and opcode contracts are in [wire protocol v2](../spec/wire-protocol-v2.md); flag defaults and
`--dump-config` keys are in the [CLI reference](../cli.md).

No Prometheus or OpenTelemetry exporter exists. Histogram approximations are diagnostics, not SLOs
(`GATE-TELEMETRY` residual). The claim ceiling is architectural prototype.

## Probes

Wire opcodes 7–9 are accepted before `INIT`/`BIND_WORKER` and do not mutate Store state
([wire protocol v2 §4](../spec/wire-protocol-v2.md)). Any v2 client that can send raw opcodes can
probe: the official SDKs encode `HEALTH`, `READY` and `STATS`, and there is no separate probe
binary.

| Opcode | Success value | Meaning |
| --- | --- | --- |
| `HEALTH` (7) | `GlifiStore/live` | Process and executors are live (`started && !failed_`); not paired Writer sticky health |
| `READY` (8) | `GlifiStore/ready` | Safe for new traffic: Store admission open, not shutting down, catalog and pair Writers healthy, maintenance neither in emergency nor in a sticky fault |
| `STATS` (9) | Bounded ASCII `GlifiStore/stats` plus `key=value` lines | Admin snapshot: version, live/ready, connections, durable lane and batch counters, per-lane latency histograms (`queue_wait_ns`, `service_ns`), maintenance fields |

- Orchestrators and load balancers must gate traffic on `READY`, not on `HEALTH` alone.
- During drain, `READY` fails while `HEALTH` may still succeed briefly. A sticky storage fault can
  also leave `HEALTH` succeeding while `READY` fails.
- A failed probe returns `INTERNAL_ERROR` with an empty value.
- `STATS` may return `OVERLOADED` if the report would exceed its size budget.
- In cleartext or default mode, protect `STATS` as a private admin surface at the listener
  boundary. With authz enabled it requires `read` for an unscoped principal and `admin` for a
  prefix-scoped principal; the secure profile supplies authenticated mTLS principals
  ([ADR 0021](../adr/0021-secure-profile-authentication.md),
  [ADR 0027](../adr/0027-stats-isolation-prefix-principals.md)).

For triage, see [drain and overload](graceful-drain-and-overload.md) and the
[durable daemon probes](durable-tcp-daemon.md#probes-and-deadlines).

## Units and cardinality

| Suffix or shape | Unit | Notes |
| --- | --- | --- |
| (no suffix) | dimensionless counter | Monotonic unless documented otherwise |
| `*_ns` | nanoseconds | Wall or monotonic, as implemented per field |
| `*_bytes` | bytes | |
| `*_bp` | basis points | Ratio × 10000 |
| boolean fields | `0` / `1` | |
| `lane[N].*` | per shard-pair Writer lane (`pair_writer_stats`) | `N` = shard-pair or executor index |
| `batch[W].*` | per durable batch shard pair | Thin subset of batch stats; the wire still labels owner ids as Workers |

Cardinality is bounded by the configured shard pairs (`STATS` `executors`) and connections; do not
scrape unbounded label sets. Manifest and wire still expose `worker_count` as the 0.1.x name for
that pair count. There is no remote metrics port: scrape through authenticated `STATS` or logs.

## `STATS` fields

`src/server/server_stats.cpp` is the authority for emission order. Minor releases may add fields;
treat unknown keys as forward-compatible.

### Process and security

`version`, `live`, `ready`, `executors`, `connections_active`, `connections_adopted`,
`output_scatter_responses`, `output_scatter_bytes`, `output_scatter_partial_writes`,
`output_scatter_completions`, `input_buffer_compactions`, `input_buffer_bytes_moved`,
`output_buffer_compactions`, `output_buffer_bytes_moved`, `cold_reads_pipelined`,
`cold_read_window_deferrals`,
`tls_enabled`, `tls_mtls`, `tls_crl`, `tls_ocsp_fail_closed`, `authz_enabled`, `authz_principals`,
`auth_accepts`, `auth_denies`, `authz_denies`, `tls_errors`.

Abuse (Phase 5): `abuse_accepts_rejected`, `abuse_idle_closed`, `abuse_request_timeout_closed`,
`abuse_connection_rate_rejected`, `abuse_principal_request_rejected`,
`abuse_principal_bandwidth_rejected`.

### Maintenance

`maintenance_state`, `maintenance_pressure`, `mutations_rejected`, `compact_attempts`,
`compact_completed`, `useful_compactions`, `maintenance_skips`, `maintenance_consecutive_no_gain`,
`maintenance_last_skip_reason`, `maintenance_last_activation_reason`,
`maintenance_last_no_gain_source_records_verified`,
`maintenance_last_no_gain_source_bytes_verified`,
`maintenance_last_no_gain_expired_records_dropped`, `maintenance_total_no_gain_*` (same three),
`maintenance_no_gain_scans_suppressed`, `maintenance_no_gain_retry_after_ns`,
`maintenance_sequence_conflicts`, `maintenance_candidate_worker`,
`maintenance_candidate_sealed_record_bytes`, `maintenance_candidate_live_record_bytes`,
`maintenance_candidate_dead_record_bytes`, `maintenance_candidate_dead_byte_ratio_bp`,
`maintenance_candidate_scheduling_dead_byte_ratio_bp`, `maintenance_unread_ttl_probe_performed`,
`maintenance_candidate_unread_expired_sealed_record_count`,
`maintenance_candidate_unread_expired_sealed_record_bytes`,
`maintenance_rate_window_bytes_copied`, `maintenance_rate_window_cpu_ns`,
`maintenance_last_compaction_pacing_delay_ns`, `maintenance_total_compaction_pacing_delay_ns`,
`maintenance_last_compaction_pacing_sleep_count`, `maintenance_last_compaction_pacing_burst_bytes`,
`maintenance_foreground_latency_samples`, `maintenance_last_foreground_p99_ns`,
`maintenance_latency_suspends`, `maintenance_latency_guard_active`,
`maintenance_latency_deferral_age_ns`, `maintenance_latency_debt_overrides`.

The enum string values for state, pressure, skip and activation are defined beside the renderer in
`server_stats.cpp`.

### Durable rotation

`durable_rotation_attempts`, `durable_rotations_committed`, `durable_rotation_compaction_waits`,
`durable_rotation_final_record_commit_attempts`, `durable_rotation_final_record_commits`, plus the
`durable_rotation_last_*`, `total_*` and `maximum_*` timing fields (`publication_wait_ns`,
`seal_ns`, `create_ns`, `manifest_publication_ns`, `execution_ns`, `total_ns`,
`final_record_commit_ns`).

[Soak](soak.md) samples often watch `useful_compactions` and `durable_rotations_committed`; short
soaks may leave them at zero.

### Paired Writer lanes (`lane[N].*`)

Epochs and queues: `reader_safe_epoch`, `writer_epoch`, `queue_depth`, `queued_bytes`,
`maximum_queue_depth`, `maximum_queued_bytes`, and the payload slot and arena admission fields.

Shutdown lifetime: `reader_shutdown_finalized` is terminal; `shutdown_generations_reclaimed` counts
every generation owner the daemon releases at the post-drain edge.

`generation_admission_backpressure_total` counts mutations rejected before the Store because the
retire bound or incremental publication capacity was exhausted. It is distinct from queue-full and
queue-wait expiry.

Flow: `admitted`, `rejected`, `expired_before_store`, `completed`, `conflict_retries`,
`conflict_retry_commits`, `writer_batches`, `writer_batch_records`,
`maximum_writer_batch_records`, `total_writer_batch_wait_ns`, `maximum_writer_batch_wait_ns`,
`writer_batch_durability_deadline_closes`, `writer_batch_queue_deadline_closes`,
`sync_drain_turns`, `sync_turn_splits`, `sync_async_fairness_turns`, `publications`,
`publication_records` and `completion_notifications`, plus read-refresh, generation, delta and merge
counters.

- The two deadline-close counters separate durability-policy batching from an earlier admission
  SLO.
- `sync_turn_splits` counts caller work continued after the 32-record quantum;
  `sync_async_fairness_turns` counts sync turns deliberately followed by queued async work.
- Compare `completion_notifications` with `completed` and `writer_batches` to quantify completion
  wakeup coalescing. A normal single-Reader pair emits one notification per non-empty Writer batch,
  after all of that batch's completions are published to the bounded SPSC queue; the counter
  measures actual wakeup calls, not completed mutations.

Merge debt gauges are `read_merge_remaining_slots` and `read_merge_post_capacity_remaining`;
`maximum_read_merge_quantum_slots` is the high-water mark of slots actually processed in one
maintenance turn. Compare them with `read_merge_post_entries`, `read_merge_slots_processed`,
`read_merge_backpressure` and foreground service p99. A falling debt with positive post capacity is
normal. Debt near zero capacity, a quantum high-water materially above the configured minimum, or
rising merge backpressure means the base size or batch shape is forcing the proportional safety
budget, and the workload needs specific tuning.

The current immutable-view footprint fields are `read_generation_base_entries`,
`read_generation_base_capacity`, `read_generation_base_record_storage_bytes`,
`read_generation_base_record_mapped_storage_bytes`, `read_generation_base_lookup_storage_bytes`,
`read_generation_base_key_bytes`, `read_generation_base_key_storage_bytes`,
`read_generation_base_pin_storage_bytes`, `read_generation_base_allocated_lower_bound_bytes`,
`read_generation_delta_capacity`, `read_generation_delta_lookup_storage_bytes`,
`read_generation_delta_allocated_lower_bound_bytes`, `read_generation_shell_bytes` and
`read_generation_current_allocated_lower_bound_bytes`.

They come from one coherent Writer-published census. The Writer maintains exact reachable delta
page, block and chunk counts and base key-block bytes while it builds those structures, so
exporting the census is O(1) and never scans the immutable index from the mutation path.
`*_allocated_lower_bound_bytes` covers explicitly sized allocation payload, not allocator metadata
or control blocks, retired generations or an in-progress merge builder. Do not read it as RSS or as
total per-key memory. For process RSS and peak RSS, final allocator in-use and reserved bytes where
the platform exposes them (macOS and glibc Linux), and the unattributed residual, use the dedicated
native memory census benchmark.

`read_generation_spare_mapping_bytes` is process-wide, not per lane. It reports the payload capacity
of at most one retired immutable-base size-class mapping retained by each base lineage, excluding
guard pages. It can fall when a lineage crosses a size class or is destroyed, and must not be added
again to a lane's current-generation total.

Latency histograms are approximations, not SLOs:

| Prefix | Meaning |
| --- | --- |
| `lane[N].queue_wait_ns.*` | Time in the mutation lane before Store entry |
| `lane[N].service_ns.*` | Store service time |

Each histogram has `.count`, `.sum` (ns), cumulative buckets `.le_1000` … `.le_1000000000` and
`.le_inf`, and approximate `.p50` and `.p99` (ns). Bucket edges run from 1 µs to 1 s, then +Inf
(`include/glifistore/core/latency_histogram.hpp`).

### Durable batch lanes (`batch[W].*`)

One durable batch surface per shard pair; `W` matches the owner (shard-pair) id, which the wire
still labels as a Worker. Fields: `enabled`, `pending_records`, `committed_batches`,
`failed_batches`.

## JSON lifecycle logs

`--log-format json` (or its config equivalent) writes JSON lines to standard error. Common fields
are `ts` (epoch seconds), `event` and `program`. String fields are capped at about 256 bytes and
carry no TLS private material.

| `event` | Notable fields | Suppressed by `--quiet` |
| --- | --- | --- |
| `start` | — | yes |
| `listen` | `bind`, ports or `unix_socket`, `executors`, `storage` | yes |
| `ready` | `ready`; if 0, `reason` | no |
| `maintenance_emergency` | `maintenance_pressure` | no |
| `maintenance_fault` | `maintenance_state`, `error_code`, `error_message` | no |
| `shutdown_begin` | optional `signal`, `executor_failure` | no |
| `shutdown_drain_begin` | `drain_ms` | no |
| `shutdown_drain_end` | `timed_out`; optional `join_failed` | no |
| `stopped` | optional `signal` | yes |
| `executor_failure` | `error_code`, `error_message` | no |

`ready.reason` is one of `not_live`, `shutting_down`, `store_not_operational`, `pair_fail_closed`,
`admission_fenced`, `maintenance_emergency` or `maintenance_fault`.

## Security audit events

With `--secure-profile` or JSON logging, `auth`, `authz` and `tls` audit events may also appear on
standard error, without payloads ([secure profile](../security/secure-profile.md)). Emission is off
by default otherwise. `audit_events_suppressed`, `audit_events_written`, `audit_events_pending` and
`audit_events_write_failed` expose the bounded best-effort delivery; snapshots are concurrent.

`SecurityAudit` increments atomic decision counters, which feed `STATS`, and uses try_lock to submit
bounded JSON events to one process-lifetime emitter. Sink I/O never runs on Readers or during Server
destruction. The fixed queue has 128 slots plus one in-flight event, and admission and write
attempts are each capped at 128 per second. A blocked sink suppresses later events, and process
exit may lose pending events.

Audit and telemetry are not a mutation hot-path SLO: deny storms are correctness and coherence
tests, not latency budgets. In load-sensitive lab runs where only the counters matter, prefer quiet
or disabled audit emission.

## Effective configuration

`glifistored … --dump-config` prints `GlifiStore/config` and exits without listening. Use it to
capture the effective observability-related settings: `log-format`, `quiet`, abuse limits,
maintenance rate and p99 guards, durable mutation queue caps, `shutdown-drain-ms`, and TLS and authz
flags (paths only).

## Alerts

| Intent | Signal |
| --- | --- |
| Liveness | `HEALTH`, or log `listen` / `stopped` |
| Traffic readiness | `READY` and log `ready` |
| Overload | wire `OVERLOADED`; `STATS` `lane[N].rejected`, `abuse_*`, queue depths |
| Maintenance gate | `mutations_rejected`, `maintenance_pressure`, log `maintenance_emergency` |
| Tail latency (diagnostic) | `lane[N].queue_wait_ns.p99`, `lane[N].service_ns.p99`, `maintenance_last_foreground_p99_ns` |
| Durability progress | `durable_rotations_committed`, `useful_compactions` (may be 0 on short soaks) |
| Current read-view footprint | `lane[N].read_generation_current_allocated_lower_bound_bytes` plus its base and delta components; compare separately with process RSS |

Do not publish absolute ops/s or p99 product claims from hosted CI
([performance budgets](../assurance/performance-budgets.md)).

## Metrics exporters

There is intentionally no first-party OpenMetrics or Prometheus HTTP scrape endpoint. Operator
telemetry is:

- wire `HEALTH`, `READY` and `STATS`, authenticated and capability-gated as configured;
- standard-error JSON lifecycle and security audit lines;
- `--dump-config` for the effective settings.

An exporter would be a new surface (listen port, authentication, cardinality contracts, scrape SLO)
and is not on the architectural-prototype claim path. The residual on `GATE-TELEMETRY` and
`GS-OPS-CONFIG-001` stays: histogram approximations are diagnostics, not SLOs, and the absence of an
exporter is explicit, not an unfinished stub.

## Related

- [Operations](README.md), [server model](../architecture/server-model.md)
- Implementation: `src/server/server_stats.cpp`, `src/server/daemon_log.cpp`
- [Soak profiles](soak.md): software soak limits and the `SIGTERM` drain
- [Secure profile](../security/secure-profile.md): audit emitter bounds and Phase 8 residuals
