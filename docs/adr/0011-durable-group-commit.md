# ADR 0011: Durable group commit

- Status: accepted
- Date: 2026-07-15

The alpha storage engine adds `durable_group` and shared Segment batching used by `durable_periodic`.
ADR 0008 remains authoritative for per-record strict semantics. ADR 0010 remains authoritative for
periodic visibility semantics.

## Motivation

`durable_periodic` removed per-write synchronization but still performs one commit-slot `pwrite`
per record. Group commit batches multiple records into one commit-slot publication and shares the
ordered Record and commit-slot persistence phases across the complete batch, reducing overhead on
both strict and periodic paths without weakening Record-before-slot ordering.

## Decision

### Shared Segment batching

`DurableSegmentFile` separates logical append state from the last persisted commit slot:

1. `append_record` writes only Record bytes and advances logical metadata in memory.
2. `flush_pending_commit` establishes the platform Record-before-slot ordering boundary, publishes
   one commit slot, and optionally synchronizes that slot immediately.
3. `commit_generation` advances once per batch flush, not once per Record.

### `durable_group`

Add `StorageMode::durable_group` with configurable `DurableGroupConfig`:

- `max_records` (default 32)
- `max_bytes` (default 65536)
- `max_wait_ms` (default 10)
- `min_records` (default 1)

Acknowledgement occurs only after the batch containing the mutation completes the Record-data
synchronization, commit-slot write, and commit-slot synchronization. Clients may block until the
batch closes by size, byte threshold, or `max_wait_ms`. There is no durability loss window.
For an owning paired Writer batch, a record/byte threshold publishes and synchronizes its commit
slot immediately; the explicit `commit_writer_batch()` boundary does the same for any residual
extent after the append loop. A single caller batch may contain multiple bounded physical groups,
and an oversized admitted record remains a singleton group. A strict threshold must never publish
as deferred: doing so could clear the pending batch and turn the subsequent explicit commit into a
no-op, violating the acknowledgement contract.
The batch commit executor publishes every staged mutation to the Worker Index and hot cache before
waking any waiter. In the v1 one-Worker path this executor is the dedicated durability coordinator;
the compatibility multi-Worker path still uses the producer that closes each Worker batch. Waiters
perform no serialized per-mutation publication work after the durability barriers, and readers
observe either the pre-batch state or the complete committed batch while holding the Worker lock.
The first Record establishes an absolute `max_wait_ms` deadline; later arrivals do not extend it.
The runtime starts at `max_records`. A deadline contracts the next record target to the occupancy
actually observed, never below `min_records`; when more producers are already admitted than fit in
the current target, the following batch grows toward that observed burst, never above
`max_records`. This changes only batch closure policy, not acknowledgement or ordering semantics.

### `durable_periodic` batching

`DurablePeriodicConfig` uses a throughput-oriented default of 4096 records, 4 MiB, and 1000 ms.
Visibility remains immediate after in-memory publication; ordered Record and commit-slot
synchronization is deferred to batch thresholds, `sync_interval_ms`, `Store::flush()`, or orderly
shutdown.

### Lifecycle

Bootstrap, manifest publication, rotation, sealing, and `Store::flush()` force
`flush_pending_commit(immediate)` before proceeding.

## Configuration

```cpp
struct DurableGroupConfig {
    std::uint32_t max_records{32};
    std::uint32_t max_bytes{65536};
    std::uint32_t max_wait_ms{10};
    std::uint32_t min_records{1};
};
```

All parameters must be greater than zero when batching is enabled, and `min_records` must not exceed
`max_records`.

## Recovery

Recovery selects the newest valid commit slot present after restart. The platform ordering boundary
prevents a surviving slot from authorizing missing Record bytes. Only completion of the final slot
synchronization is a client-visible durability guarantee. macOS uses `F_BARRIERFSYNC` for the first
boundary and `F_FULLFSYNC` for the final one; other targets conservatively retain a data sync for the
first boundary.

## Consequences

- Strict throughput improves with larger batches at the cost of batch latency.
- Batch publication is performed once by the commit executor; failure wakes every waiter and leaves the
  runtime fail-closed.
- Periodic throughput improves by eliminating per-record commit-slot writes.
- Benchmarks and crash evidence must distinguish `durable_sync`, `durable_group`, and
  `durable_periodic`.
- Strict parallel-put benchmarks can enable per-request latency sampling; scaling comparisons must
  hold client concurrency constant and report batch occupancy effects separately from saturated
  writer tests.

Normative ordering rules are specified in
[`durability-recovery.md`](../architecture/durability-recovery.md).

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
Strict blob groups batch small typed descriptors after every payload dependency is durable; payload buffers are never inserted into the Writer mutation lane.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
