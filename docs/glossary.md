# Glossary

**Normative** · architecture, public API, persistence v1, wire protocol v2 · reviewed 2026-08-26

The terms these pages use, and what each one means. A term keeps its capitalization when it names an
architectural entity: a Store, a Segment, a Record.

## Ownership and runtime

| Term | Meaning |
| --- | --- |
| Store | One logical exact-key key-space, exposed by the embedded API or the daemon. |
| Worker | The unit of key ownership and mutation serialization on disk and on the wire. A key has exactly one owner Worker for a routing epoch. In the 0.1.0 daemon a Worker is one shard pair; the Manifest and the wire still say `worker_count` and owner Worker id. |
| Shard pair (`ShardPair`) | One Reader role and one serial mutation-executor (Writer) role for a single owner id ([ADR 0031](adr/paired-reader-writer-shards.md), [ADR 0032](adr/0032-paired-concurrency-embedded-store.md)). The default for embedded `Store::open` and the only `glifistored` 0.1.x runtime. |
| `ShardPairRuntime` | The library-owned paired runtime inside `glifistore_core`: immutable generation publication, synchronous combining and optional bounded asynchronous lanes. Embedded callers combine synchronously; the daemon runs a dedicated Writer thread. |
| Reader | The half of a shard pair that serves GET from a local immutable `ReadGeneration`: the readiness loop (Reactor) in the daemon, the caller thread adopting the published generation when embedded. |
| Writer | The serial mutation-executor half of a shard pair, and the only append and publication path for its owner. It is reached through the embedded synchronous combiner or the daemon's bounded asynchronous lane. |
| `ReadGeneration` | The immutable publication descriptor for ordinary paired GET. Daemon Readers adopt it once per event-loop turn (borrowed or leased); public `Store::get` still returns an owning `OwnedValue` ([ADR 0009](adr/0009-public-read-ownership.md)). |
| `StoreConcurrencyMode` | The immutable open-time choice: `paired` (default) or the deprecated `legacy_mutex` (a 0.1.x escape hatch, removed in 0.2). Callers cannot switch a live Store; a detected internal ownership violation fails closed. |
| executor | A server thread that runs the Reader of one shard pair (once called "Reactor + Worker"). It is not a separate Store. |
| Reactor | The Reader's readiness loop: owner of a set of network sockets and their connection buffers. |
| owner-bound | Said of a connection or operation that executes on the Worker (shard pair) the routing function selects. |
| routing hash | A deterministic hash over the complete binary key: FNV-1a-64 by default, or persisted SipHash-2-4 when keyed routing is selected. Algorithm and seed are part of the routing identity. |
| routing epoch | The persisted identifier of one key-to-Worker ownership assignment. It changes only through an explicit migration. |
| data plane | Exact-key get, put and erase, and the Index and Segment work they immediately require. |
| control plane | Allocation, routing configuration, catalog publication, lifecycle and global diagnostics. |
| maintenance plane | Explicit compaction, verification, retirement and future vacuum scheduling. |

## Storage

| Term | Meaning |
| --- | --- |
| Index | Derived acceleration state mapping a complete key to one `RecordRef`, partitioned physically by Worker. |
| Segment | A fixed 64 MiB append unit: a 4 KiB header followed by immutable Records. |
| active Segment | The one Segment a Worker may append to. |
| sealed Segment | A Segment whose committed extent will not grow again. |
| retired Segment | A Segment removed from current authority, not necessarily safe to reuse or delete. |
| reclaimable Segment | A retired Segment with no reader, retention or recovery obligation. |
| Record | One immutable, checksummed, canonical encoding of a key and value, or of a tombstone. |
| `RecordRef` | A positional reference: Segment identity, offset, size, sequence and generation. Never a process address. |
| tombstone | An erase Record whose sequence suppresses every older Record for the same complete key. |
| canonical encoding | Exactly one accepted byte representation for one logical object, including required zero padding and reserved fields. |
| snapshot | An owning copy of diagnostic or catalog state, or a specifically pinned historical read. A returned reference to a live container is not a snapshot. |
| pin | An ownership token that prevents reclaiming or unmapping a specific Segment generation. Public pinned reads are not implemented. |
| residency | Whether Segment bytes are resident, mapped or must be loaded. Persistence alone does not imply RAM residency. |
| hot-record cache | The legacy durable-runtime RAM cache of recently published values. Never recovery authority; disabled as a read authority in default paired mode. |
| vacuum | The historical, internal name for copy-build-validate-publish-reclaim compaction. Not a public API type. |
| durable compaction | Crash-safe replacement of the oldest sealed Segments of one Worker, from one Segment up to its complete sealed set, under a persisted intent ([ADR 0044](adr/0044-bounded-prefix-compaction-remap.md)). |
| retirement | Removal of old physical objects once a newer authority is durable and no reader can require them. |
| write amplification | Physical bytes written divided by logical mutation bytes or, for the current compaction gate, preallocated replacement bytes divided by Segment bytes reclaimed. Every metric must state which definition it uses. |
| space amplification | Physical bytes retained divided by the bytes the currently visible logical state requires. |

## Durability and recovery

| Term | Meaning |
| --- | --- |
| Manifest | The authoritative durable catalog: format versions, routing configuration, and active and sealed Segment roles. |
| commit slot | One of two independently checksummed Segment-header records that authorizes a committed byte extent. |
| committed extent | The Record byte range the selected valid commit slot authorizes. |
| crash tail | Bytes beyond the committed extent. Recovery never interprets them. |
| recovery authority | The durable objects allowed to determine recovered state: the authoritative Manifest and its committed Segment extents, plus a validated recovery intent where specified. |
| visibility point | The transition after which a later admitted read may observe a mutation in the running process. |
| durable commit point | The transition after which recovery must include the mutation, subject to the documented platform storage contract. |
| acknowledgement point | The transition after which a caller may receive success. It depends on the storage mode. |
| durable-sync | Strict per-mutation acknowledgement after the durable Segment commit slot is synchronized. |
| durable-group | Strict acknowledgement after one batch shares Record ordering and commit-slot synchronization. |
| durable-periodic | Immediate in-process publication; restart durability only after a successful explicit or background flush. |
| fail-closed | The state in which an instance rejects further service because it cannot prove coherent behavior. |
| poisoned directory | A `DataDirectory` whose publication outcome became indeterminate; every handle sharing its health state rejects further mutation. |
| not committed | A failure known to precede the persistent authority change; recovery keeps the previous state. |
| indeterminate | The authority-changing operation was attempted and its durable result cannot be inferred in the running process; close and recover. |

## Operations and evidence

| Term | Meaning |
| --- | --- |
| maintenance rate budget | Pre-intent physical-write pacing (`max_copy_bytes_per_sec`) plus a one-second adjacent-job and CPU controller window (`max_cpu_ms_per_window`) for normal-mode Store compaction. Not a whole-transaction size cap, and distinct from the daemon's connection and handshake abuse controls and from E3/E4 power-loss certification. Zero disables it; pressure and emergency modes bypass it. |
| secure profile | The opt-in, fail-closed daemon posture: TLS 1.3, mTLS and a default-deny `--authz-map`; it refuses dual cleartext (`--tls-port`) and adds the abuse-limit defaults, JSON authentication audit events and optional `--tls-crl`. |
| E2 / E3 / E4 durability | Evidence levels defined by [platform durability evidence](architecture/platform-durability-evidence.md). The current durable claim is E2 (process kill); E3 and E4 sudden power loss remain open. The in-repository E3 block-reset harness is rehearsal only (`e3_certified=no`). FreeBSD CI is a portability signal, not storage certification. |
| fuzz and soak smoke | Short gates, not multi-hour hardware proof: the default CI fuzzing, `soak_daemon.sh --profile smoke` (about 45 s) and the weekly `long` profile (30 minutes). In CI, the optional `1h` and `4h` profiles run only in `soak-extended.yml`, on demand or monthly. |
| worker-affine benchmark | A workload that gives each client thread keys owned by one Worker while using the public Store path. |
| owner-bound benchmark | A workload on an internal path that has already checked ownership. Never compare it as if it measured the public locking path. |
| experimental paired prototype | The lab-only volatile TCP engine under `src/experimental/`, for microbenchmarks and archive only. Not installed, not selectable by `glifistored`, and not a second product runtime beside `ShardPairRuntime` ([ADR 0032](adr/0032-paired-concurrency-embedded-store.md)). |
