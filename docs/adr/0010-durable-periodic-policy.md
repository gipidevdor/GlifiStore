# ADR 0010: Durable periodic policy

- Status: accepted
- Date: 2026-07-14

The alpha storage engine now supports three explicit modes: `volatile`, `durable_sync`, and
`durable_periodic`. ADR 0008 remains authoritative for strict durable-sync semantics. This ADR
defines the production-oriented periodic mode and its documented loss window.

## Motivation

`durable_sync` acknowledges a mutation only after the Segment commit slot reaches the platform
synchronization boundary (`F_FULLFSYNC` on macOS, `fdatasync`/`fsync` elsewhere). That contract is
correct but limits throughput to roughly hundreds of writes per second on typical hardware. Production
deployments that accept a bounded durability window need an explicit, documented alternative rather
than weakening strict mode.

## Decision

Add `StorageMode::durable_periodic` with configurable `sync_interval_ms` (default 1000). In this
mode:

1. Record bytes are appended without an immediate file synchronization or commit-slot publication.
2. The client receives success after in-memory publication (Index and hot-record cache), not after
   disk synchronization.
3. A batch close or background flush synchronizes accumulated Record data before publishing the
   alternate commit slot, then synchronizes that slot. Explicit `Store::flush()` and orderly
   shutdown force the same ordered barrier.
4. A completed flush is the only durability guarantee. An unflushed mutation may survive if the
   platform persisted all required writes, but clients cannot rely on that outcome.

Lifecycle operations that change catalog authority — bootstrap, manifest publication, Segment
creation, rotation, and sealing — remain on the strict synchronized path defined in ADR 0008.

## Acknowledgement contract

| Aspect | `durable_sync` | `durable_periodic` |
| --- | --- | --- |
| `put`/`erase` ack | After commit-slot synchronization | After in-memory publication |
| Read-after-write | Immediate in RAM | Immediate in RAM |
| Survives restart | All acknowledged mutations | All flushed mutations; later mutations unspecified |
| Loss window | None | Up to `sync_interval_ms` plus in-flight unflushed writes |
| Lifecycle ops | Synchronized immediately | Synchronized immediately |

A client that receives `OK` in `durable_periodic` may lose the mutation if the process crashes
before the next flush. That behavior is documented, not a defect. Clients requiring strict
durability must use `durable_sync` or call `Store::flush()` and verify before assuming restart
survival.

## Configuration

```cpp
struct DurablePeriodicConfig {
    std::uint32_t sync_interval_ms{1000};
    std::optional<DurableGroupConfig> batch{
        DurableGroupConfig{.max_records = 4096, .max_bytes = 4194304, .max_wait_ms = 1000}};
};
```

`sync_interval_ms` must be greater than zero. Use `durable_sync` when the interval should be zero
(every write synchronized).

## Shutdown and manual flush

`Store::flush()` synchronizes all dirty Segment files before returning. `Store::close()` performs
the final flush, returns its sticky result, stops background execution, and releases the
data-directory lock. The destructor invokes close as a non-observable fallback. Flush failure is
fail-closed: the Store must not continue serving if it cannot make previously accepted periodic
writes durable on orderly shutdown.

## Recovery

Recovery behavior is unchanged. It selects the newest valid commit slot present after restart.
Because Record data is synchronized before a slot is published, observing a newer slot cannot make
that slot refer to an unsynchronized Record extent. Clients still receive no survival guarantee
until the slot synchronization completes.

## Consequences

- Deployment-oriented experiments should prefer `durable_periodic` unless strict per-write
  durability is required.
- Benchmarks and crash evidence must distinguish strict and periodic modes.
- Group commit is specified in [ADR 0011](0011-durable-group-commit.md) and shared with optional
  `durable-periodic` batching.

The normative ordering rules are specified in
[`durability-recovery.md`](../architecture/durability-recovery.md).

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
Blob payload file/name durability is strict before descriptor admission. Only the logical descriptor uses the existing periodic loss window; a selected descriptor never authorizes a partial payload.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
