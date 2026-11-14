# Code tour

**Descriptive** · repository version 0.1.x · reviewed 2026-07-19

Where each responsibility lives in the repository and how a put, a recovery and a TCP request travel
through the code, for a contributor about to read or change it.

## Reading order

Start with the [architecture specification](../spec/architecture.md), the [glossary](../glossary.md)
and the public `Store` declaration. Then follow one volatile operation, one durable operation,
recovery and finally the TCP adapter. Read the exact binary format documents before changing any
codec.

## Repository map

| Area | Public headers | Implementation | Responsibility |
| --- | --- | --- | --- |
| Core | `include/glifistore/core` | Mostly header-only | Result and error types, checked arithmetic, key hash |
| Store | `include/glifistore/store` | `src/store` | Public facade, configuration, lifecycle, routing, runtime selection |
| Paired runtime | `include/glifistore/store/paired` | `src/store/paired` | Shard-pair runtime: mutation lanes, Writer turns, immutable read generations |
| Worker | `include/glifistore/worker` | `src/worker` | Volatile ownership and per-Worker serialized operations |
| Index | `include/glifistore/index` | `src/index` | Swiss-style map and long-key arena |
| Segment and Record | `include/glifistore/segment` | `src/segment` | Record codec, CRC32C, in-memory Segment and manager |
| Persistence | `include/glifistore/persistence` | `src/persistence` | Manifest, file Segment, recovery, namespace, flush, compaction |
| Networking | `include/glifistore/server` | `src/server` | Sockets, pollers, Reactor, framing, executor handoff |
| TCP client | `include/glifistore/client` | `src/client` | Synchronous wire-v2 client with bounded pipelines |
| C ABI | `include/glifistore/abi` | `src/abi` | Versioned C facade over `Store` ([C ABI v1](../spec/c-abi-v1.md)) |
| Vacuum | `include/glifistore/vacuum` | `src/vacuum` | Volatile maintenance and reclamation policy |
| Experimental | — | `src/experimental` | Lab-only volatile paired-shard prototype and slot-pool candidate, built only into tests and benchmarks |
| CLI and daemon | — | `src/main.cpp`, `src/cli`, `src/server/daemon_main.cpp` | Executable entry points |
| Benchmarks | `benchmarks` | `benchmarks` | Isolated engine and loopback TCP measurement |
| Tests | `tests` | `tests` | Unit, integration, property, crash, quality and consumer coverage |

## Follow a volatile put

The default runtime is the paired Reader–Writer shard
([ADR 0031](../adr/paired-reader-writer-shards.md)).

1. `Store::put` hashes the key, routes it to the owning shard and admits the operation.
2. `ShardPairRuntime::mutate` queues the mutation on the shard's synchronous lane. An embedded Store
   without an asynchronous lane runs the Writer turn on a calling thread that takes the shard's
   execution token and also processes the queued mutations of other callers
   ([ADR 0037](../adr/0037-shard-execution-token-flat-combining.md)). With an asynchronous lane,
   as in `glifistored`, the shard's dedicated Writer thread runs it.
3. The Writer appends an encoded Record to the Worker's active Segment and publishes the returned
   `RecordRef` in the Worker Index, without taking the Worker mutex.
4. The same turn publishes a new immutable read generation covering up to 32 queued mutations, then
   completes the call. A GET adopts the current generation and does not take the Worker mutex.
5. Rotation goes through `GlobalSegmentManager` when the active Segment is full.
6. The admission guard is released on every exit path.

The critical invariant is **append complete before Index publication**. The deprecated
`legacy_mutex` mode ([policy](../architecture/legacy-mutex-policy.md)) bypasses the shard runtime:
the Worker locks its mutex, appends and publishes in the Index, and there is no read generation.

## Follow a durable put

The same facade selects the internal durable runtime. The owning durable Worker prepares its Index,
encodes into the active file Segment and follows the selected sync, periodic or group commit order.
`DurableRuntimeCatalog` resolves Segment identity and file access; the Manifest-publication mutex is
entered only for namespace transitions such as rotation.

Do not assume that the volatile `Worker` class is also the durable runtime Worker. They implement
the same ownership model with different storage authorities.

## Follow recovery

Read in this order:

1. filesystem and namespace audit;
2. bootstrap and compaction intent resolution;
3. Manifest decoding and catalog validation;
4. Segment-file header and commit-slot validation;
5. committed Record scan;
6. latest-key rebuild and per-Worker Index creation;
7. runtime catalog publication.

Recovery code must be reviewed against [persistence v1](../spec/persistence-v1.md), never only
against the happy-path writer.

## Follow a TCP request

The daemon configures a `Store` and a `Server`. The acceptor assigns a new connection to an
executor, and the Reactor decodes protocol frames. `INIT` reports capabilities; `BIND_WORKER` may
hand the connection object through a bounded MPSC queue to the target executor. From then on that
executor owns the connection and invokes the Store. Wrong-owner operations receive routing metadata
rather than remote execution.

Platform pollers are separate implementations: kqueue on macOS, FreeBSD and OpenBSD, epoll on Linux.
Their observable contract is the Reactor interface, not the platform event struct layout.

## Change behavior safely

| Change | Required with it |
| --- | --- |
| Public semantics | Update the public header, the [C++ API reference](../reference/cpp-api.md) and the tests together |
| Index algorithm | Update [Index v1](../spec/index-v1.md) if semantics or invariants change; pure SIMD equivalence need not version it |
| Persistent bytes | Update the format version, fixture, compatibility matrix, recovery and ADR before merging |
| Wire bytes | Update the protocol version and specification, and the golden tests |
| Locks and atomics | Update concurrency ownership and lock order |
| Benchmarks | Preserve sample validation and document any timed-region change |

## Common traps

- Confusing a `RecordRef` with lifetime ownership.
- Returning a reference to a container after releasing its mutex.
- Treating a filename, cache or Index as durable authority.
- Hashing a key differently for routing and then searching other Workers on a miss.
- Publishing a commit slot before Record bytes are ordered.
- Holding the flush coordinator mutex while running its callback.
- Moving a connection without relinquishing the source executor's access.
- Comparing owner-bound internal benchmarks with public Store benchmarks.
- Changing a reserved field from zero without a compatibility decision.

## Before submitting a change

Run the focused tests, the full test suite, the formatter and the appropriate sanitizer. Persistent
and network changes require fixture and malformed-input coverage. Performance changes require the
standard benchmark matrix and raw results. Update the normative document whose contract changed; do
not bury the decision only in a code comment.
