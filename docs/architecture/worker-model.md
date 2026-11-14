# Worker model

**Descriptive** · paired Store, volatile and durable backends

How a Store sizes its Workers, what each Worker owns and how keys reach it. Workers are an internal
execution and ownership mechanism, not separate user-visible stores.

## Sizing

At startup, `WorkerTopology` detects logical CPUs, usable CPUs, physical cores where available, and
memory. `WorkerCountPolicy` applies the explicit override, reserved cores, maximum Worker count and
minimum memory per Worker. The result is always at least one and stays stable for the process
lifetime. For a durable Store, automatic selection happens only at creation; reopen uses the
persisted Worker count and routing configuration.

Detection differs by platform. macOS detects physical cores and memory with `sysctlbyname`. FreeBSD
uses `kern.smp.cores` and `hw.physmem`; OpenBSD uses `_SC_NPROCESSORS_ONLN` for the available CPU
count. Linux uses the online logical CPU count and a `sysinfo` free-memory sample; it does not yet
distinguish physical cores or reduce the count to the process affinity mask. Every platform keeps
the portable `std::thread::hardware_concurrency` fallback. An explicit Worker (shard pair) count is
therefore the deterministic choice for controlled affinity or benchmark runs.

## Ownership

Each Worker owns:

- one physical Index partition;
- one active Segment at a time;
- a monotonic sequence counter;
- references to the Segments it produced;
- Worker-local accounting and maintenance state.

The volatile `Worker` owns in-memory Segments through `GlobalSegmentManager`. Durable mode uses a
separate internal runtime Worker that owns a file-backed active Segment and bounded hot-record state
through `DurableRuntimeCatalog`. The two share the routing and ownership contract, not one concrete
class.

Each Worker resolves Records through its local catalog of owned Segments, so the hot data path does
not consult the global catalog. `GlobalSegmentManager` is the control plane for Segment allocation,
rotation, retirement and global snapshots.

## Routing

Routing is deterministic from the persisted routing algorithm and seed, and routes directly by
Worker count. Routing ownership is authoritative: a miss in the routed Worker or generation is a
Store miss, not a request to search peer Workers.

Online Worker resizing would require a stable routing transition and migration protocol, and is
intentionally outside 0.1.x. The persisted Worker count changes only through offline
[store migration](store-migration.md) ([ADR 0024](../adr/0024-offline-worker-migration.md)). Future
rebalancing must transfer ownership through an explicit routing-table state transition; it must not
introduce peer-to-peer fallback searches in the normal data path.

## Concurrency

The public `Store` API (`get`, `put`, `erase`) is thread-safe. The product default is paired
concurrency ([ADR 0032](../adr/0032-paired-concurrency-embedded-store.md)). Each key routes to one
owner shard. An ordinary GET adopts that shard's immutable `ReadGeneration` without taking the Index
mutex, and `put` and `erase` hand off to the shard's serial Writer. Independent keys on different
shards run in parallel; mutations on the same shard serialize on its Writer lane.

The deprecated `StoreConcurrencyMode::legacy_mutex` restores the historical path, which serializes
callers on the Worker's mutex. Open refuses a Store that would mix legacy mutex mutators with a
paired Writer.

For maintenance, `verify_index()` acquires every Worker mutex in ascending index order before it
scans Segments or compares Index partitions. Direct use of `Worker::index()` bypasses that
synchronization and is meant only for single-threaded tests and tools. Compaction, backup and
durable catalog refresh keep their existing locks under paired mode. The complete lock order, atomic
publication rules, operation admission and shutdown model are normative in the
[concurrency and memory model](../spec/concurrency-memory-model.md).

The supported public API returns an owning value copy. Internal server execution may use a
`RecordView` only through the build-tree-only Store access bridge, and must encode the response
before its execution lifetime ends. An explicitly pinned public zero-copy handle is future work; see
the [public C++ API reference](../reference/cpp-api.md).
