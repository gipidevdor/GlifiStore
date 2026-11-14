# Volatile paired-shard prototype

**Descriptive** · volatile Reader–Writer prototype and single-pair TCP Reactor under `src/experimental/` · reviewed 2026-08-27

Decisions: [paired Reader–Writer shards](../adr/paired-reader-writer-shards.md) (ADR 0031);
[ADR 0036](../adr/0036-generation-slot-pool-publish.md) (proposed) governs the generation slot pool.
Its production implementation is an embedded opt-in, off by default
(`PairedConcurrencyConfig::generation_slot_pool`).

What the experimental, lab-only volatile prototype implements, what it measured and where it stops.
It runs only in dedicated tests and benchmarks and is not reachable from `glifistored`.

## Implemented scope

The prototype runs a single pair with:

- a Reader-owner caller and a persistent Writer `std::thread` with explicit stop/join;
- 256-element SPSC mutation and completion rings;
- producer and consumer indices separated on 128-byte alignment;
- a pool of 256 mutation slots and a contiguous value arena, allocated at startup;
- an implicit completion credit in the slot: a slot becomes free only after the completion pop;
- Writer micro-batches of up to 32 mutations, configurable by record count and deadline; the
  balanced default profile uses 32 records and 2 µs, and the benchmark exposes both thresholds so
  throughput and visibility latency are not confused;
- a Writer-private `MutableDeltaIndex` and `base + frozen delta` publication;
- delta merge at the configured threshold, with no chain deeper than two levels;
- publication through a single release/acquire atomic token `{epoch:48, slot+1:16}`; the token also
  identifies the slot's reincarnation, and the immutable descriptor in the slot holds `generation`,
  `epoch` and `visible_through`;
- a read-only Swiss-style `ImmutableReadIndex` with eight-slot control groups, a maximum load of
  0.75, the SIMD/scalar matching already shared with the production Index, and no tombstones;
- one `StableRecord` allocated per mutation; base, delta and generation share only compact handles,
  and GET does not modify the reference count;
- a persistent delta of immutable 16-slot pages: small deltas stay flat, while large ones separate a
  persistent two-level ownership directory from a flat view of non-owning pointers. The
  `ReadGeneration` pins ownership and GET keeps direct page access;
- a pool of 258 generation slots and QSBR based on Reader turns, with no reference counting on GET
  or adoption;
- PUT, ERASE, TTL, read-after-write and shutdown drain;
- metrics for queue, batch, publication latency and storage, ingress copy and generation retirement.

The second stage connects the pair to an experimental cleartext TCP Reactor:

- protocol v2, compatible with the public C++ client, for `INIT`, `BIND_WORKER`, `PING`, `GET`,
  `PUT`, `ERASE`, `HEALTH` and `READY` on a single pair;
- a bounded parser, input and output watermarks and connection slots;
- an ordered pipeline with at most one mutation awaiting completion per connection;
- GET responses through `sendmsg` scatter/gather, with header and value never concatenated;
- a generation pin only when socket output is partial, released after the last byte;
- a metrics snapshot published at the end of each event-loop turn, so cross-thread reads never race
  on Reader-local counters and add no atomics or mutexes to ordinary GET;
- a native Writer→Reader wakeup once per completion batch;
- telemetry for requests, response bytes, `sendmsg`, partial writes, pins and backpressure.

GET uses only the local descriptor adopted by the Reader. Tests verify that 1,000 GETs increment the
push or pop count of no ring.

## Containment

`paired_shard.cpp` and `paired_reactor.cpp` are compiled directly into `glifistore_tests` and the
`glifistore_paired_benchmark` and `glifistore_paired_reactor_benchmark` targets. They do not belong
to `glifistore_core` or `glifistore_server_core`, are not installed and are not reachable from
`glifistored`. The prototype opens no persistent Segment and changes neither the Store, wire v2 nor
persistence v1. This separation prevents accidental activation of the incomplete runtime.

## Reclamation

The Reader increments a quiescent-turn counter before adopting the current token with acquire. The
Writer fully initializes the generation and its descriptor in the slot, then publishes one packed
token with release. The Reader decodes the slot and checks that the descriptor's epoch matches the
token's, so reuse of the same address cannot impersonate the same publication (ABA). The Writer
retires the previous generation only after two further quiescent boundaries; that two-turn margin
covers a concurrent adoption that may still have observed the previous descriptor. The span returned
by GET stays valid until the same Reader's next `adopt_publication()`.

The pool holds `queue_capacity + 2` slots: an explicit budget of 258 concurrent generations. This is
not a promise that every admitted mutation keeps publishing if the Reader stops advancing
indefinitely, because a completion pop can reopen admission while publication debt grows. When the
budget is exhausted, the Writer makes one bounded-wait reclaim attempt and completes the batch with
`resource_exhausted`; it never overwrites a live slot and never waits without bound. During
shutdown, two real turns unblock reclaim of the existing debt, and join still classifies every
admitted mutation with an explicit completion.

## Intentional limits

- The request arena and the generation pool are bounded, but every new version still allocates a
  `StableRecord`, the touched delta pages and the publication structures. A general-purpose PMR and
  a Writer-only `StableRecord` slab were measured and rejected: both worsened 95/5, and the slab
  retained payload capacity up to its high-watermark. A production allocator needs size classes and
  a retention policy, not one pool applied indiscriminately.
- The Writer still uses yield polling for idle admission; completion return already uses the
  Poller's native wakeup. Affinity, adaptive spin/park and profiles are not implemented yet.
- The Reactor is cleartext, volatile and single-pair: it implements no TLS, handoff, durable cold
  read, Segment v1, durability, rotation, recovery or compaction.
- The prototype implements one pair and certifies no scaling.
- `glifistore_paired_benchmark` remains an engine microbenchmark.
  `glifistore_paired_reactor_benchmark` is wire-to-wire, uses the same public client and interleaves
  the current baseline with the prototype. Its TCP latencies are batch-completion latencies, not yet
  separate per-GET and per-PUT histograms.

The first A/B results therefore qualify only the data path's potential. An architectural success
still requires the same protocol path, immutable Segments, durability and multiple pairs.

## Evidence

Tests:

- full Debug suite: 446 tests, pass;
- full ASan+UBSan suite: 446 tests, pass;
- full TSan suite: 446 tests, pass. The first TCP run found a race in metrics reads; the
  turn-boundary snapshot removed it, and the full rerun is clean;
- full Debug, ASan+UBSan and TSan suites on the persistent directory and pinned view: pass
  (441 tests);
- a concurrent SPSC test with 100,000 elements and wraparound;
- saturation of all 256 completion credits without loss;
- slot reuse after drain;
- publication, ACK and read-after-write;
- repeated merges, tombstones, TTL and shutdown drain;
- 512 consecutive publications with bounded QSBR reclamation and no backpressure;
- repeated slot reincarnation with a consistent `{epoch, slot}` token, verified in Release,
  ASan+UBSan and TSan; the `generation_slot_reuses` counter proves the test does not exercise only
  never-reused slots;
- TCP tests with the public C++ client for CRUD, read-after-write and ordered pipelines;
- a deterministic slow-output test: partial write, observed generation pin and release on drain.

Measurements:

- interleaved A/B benchmarks for 100% GET and 95/5 GET/PUT in the dedicated
  `glifistore_paired_benchmark` target;
- second gate: the paged delta and stable records bring 95/5 to 11.30 Mops/s (64 B) and
  10.90 Mops/s (1 KiB), 2.15× and 4.57× the baseline respectively;
- the persistent ownership directory with a flat Reader view cuts owning-handle copies by about 65%
  in the 4,096-key profile; the 2 µs deadline keeps the balanced gate, while 8 µs reaches
  11.59/11.55 Mops/s mixed at 64 B/1 KiB under a different visibility contract;
- interleaved TCP A/B benchmarks between the current Reactor and the paired Reactor, with throughput
  and batch p50/p99/p99.9 plus publication and output telemetry;
- TCP GET 64 B, pipeline 32: +0.6% to +4.8% for 1–8 connections, and batch p99 does not improve
  uniformly. At pipeline 128 the throughput gain is +11.4%, while pipeline 1/8 stays within a 2.5%
  regression;
- TCP GET 1 KiB/64 KiB/256 KiB: +21%, +233% and +221%; for large values scatter/gather also cuts
  batch p99 by 64–80%, and the test observes pins only on partial writes;
- mixed TCP 64 B, four connections, pipeline 32: −18% at 99/1, −54% at 95/5 and −73% at 90/10. The
  mean publication batch is only 1.07/1.25/1.43 records: handoff, publication and the
  per-connection ordering barrier dominate the small value.

## Next steps

The prototype under `src/experimental/` stays **lab-only**: it is not a second selectable runtime
and will not be promoted into `glifistored`. The daemon already runs the paired model
([ADR 0031](../adr/paired-reader-writer-shards.md), [server model](server-model.md)).

The remaining adoption and performance items are gates of the
[paired shards benchmark plan](../benchmarks/paired-shards-plan.md), not a cutover of the volatile
prototype:

1. Delta mixed magnitude: directory-chunk COW landed; the −1.8% can be confirmed only on
   hard-pinned Linux
   ([paired Delta directory-chunk COW](../benchmarks/paired-delta-directory-chunks-2026-07-31.md));
2. `get-into` and multi-extent scatter: rejected pending proof of a bounded win
   ([multi-extent rejection](../benchmarks/paired-get-into-multi-extent-reject-2026-07-31.md));
3. A/B at 1/2/4/8 pairs on hard-pinned Linux: harness ready, waiting on `glifistore-linux-perf`
   ([Linux hard-pinned A/B harness](../benchmarks/paired-shards-linux-p1.md));
4. optional Linux I/O backend: deferred for 0.1.0, with no proven win free of ordering risk.

Lab work on the volatile TCP prototype (separate GET and PUT histograms, experimental affinity, a
size-class allocator) may continue in the dedicated targets; it is not a daemon integration gate.
