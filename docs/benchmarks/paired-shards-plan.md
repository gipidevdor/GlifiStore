# Benchmark plan for paired Reader–Writer shards

**Roadmap** · paired Reader–Writer shard runtime, benchmark gates

Current state: the paired runtime is the only runtime for 0.1.0; output lease, mutation arena and
Delta arena are closed.

ADR: [paired Reader–Writer shards](../adr/paired-reader-writer-shards.md)

Initial baseline: the current runtime on `main`, same persistence v1 and wire v2.

Results of 2026-07-28: `benchmark-results/paired-shards/fc9463d-dirty/macos-m4/2026-07-28/`. This
gate run is explicitly versioned; ordinary local benchmark artifacts stay ignored.

Second P0 gate: `benchmark-results/paired-shards/573f4f1-dirty/macos-m4/2026-07-28-p0-paged-delta/`.

Directory/batching gate:
`benchmark-results/paired-shards/55acedd-dirty/macos-m4/2026-07-28-persistent-directory-batching/`.

Reactor TCP gate, one pair:
`benchmark-results/paired-reactor/d2077cf-dirty/macos-m4/2026-07-28-phase2-tcp/`.

## Current state — 2026-07-31

The historical sections below document the gates that preceded the migration. The daemon does not
offer two concurrent models: the Reader/Reactor and the serial Writer per shard are the **only**
runtime of `glifistored` 0.1.0. The volatile prototype was removed from the tree; the separate
ADR 0036 generation-publication laboratory remains under `src/experimental/` and is not a second
selectable model. The following are closed and covered by tests: multi-pair routing,
mutation/completion SPSC, immutable publication, refresh on rotation/compaction, compact Base Index,
incremental merge, per-pair cold-read lane, preallocated task slots, QSBR lease of the cold I/O,
adaptive cleartext output lease with short-write backpressure, bounded mutation slot/key/value
arena, and a Writer-owned generational Delta bounded on versions.

Remaining operating order (P1 — it does not reopen a dual runtime):

1. P1 — Delta mixed: candidate hierarchical directory-chunk COW landed; macOS advisory evidence in
   [`paired-delta-directory-chunks-2026-07-31.md`](paired-delta-directory-chunks-2026-07-31.md);
   magnitude vs the historical −1.8% still needs Linux hard-pinned confirmation;
2. P1 — `get-into` / scatter multi-extent: **rejected pending proof**
   ([`paired-get-into-multi-extent-reject-2026-07-31.md`](paired-get-into-multi-extent-reject-2026-07-31.md));
3. P1 — A/B 1/2/4/8 pair Linux hard-pinned: harness ready, gate **not closed on macOS**
   ([`paired-shards-linux-p1.md`](paired-shards-linux-p1.md)); runner label
   `glifistore-linux-perf` required — no fabricated pass-candidate;
4. P1 — optional Linux I/O backend (`io_uring` / batched completion): **deferred / skipped for
   0.1.0 adoption**. No measured queue/syscall win that preserves persistence write ordering and
   acknowledgement points; re-open only with a versioned A/B under the Linux hard-pinned harness
   and an ADR if ordering/ack semantics would change (they must not silently).

The most recent cold-read gate is
[`paired-scatter-output-2026-07-29.md`](paired-scatter-output-2026-07-29.md). The unpinned macOS
results remain indicative evidence, not a production claim.

The mutation task storage gate is
[`paired-mutation-arena-2026-07-29.md`](paired-mutation-arena-2026-07-29.md): it removes per-task
ownership and allocations, makes the byte budget real, and passes Debug, sanitizers, fault/crash and
the mutation-bound A/B. The 99/1 control stays neutral/inconclusive on unpinned macOS.

The final Delta gate is
[`paired-generational-delta-arena-2026-07-29.md`](paired-generational-delta-arena-2026-07-29.md): it
removes the per-version handles with 64 B cells and QSBR lifetime, also bounds churn on the same
key, reduces RSS by 2–4% and improves volatile GET. The mixed cost of about 1.8% remains an explicit
P1 and requires a hard-pinned Linux profile.

## Comparison rules

Every comparison uses two binaries from the same source base: the current runtime and the paired
runtime. Build, dataset, seed, client, filesystem, durability policy and limits must match. The
order is interleaved `old/new/new/old`; at least two cycles, one warmup and seven measured samples
per cell. The raw output, the effective configuration and the commit are kept in:

```text
benchmark-results/paired-shards/<commit>/<platform>/<run-id>/
```

A run is invalid if it loses responses, request ids, payloads, mutation outcomes or the final
verification. Client and server on the same host must have disjoint CPUs or the result is marked
combined-load.

## Minimum matrix

| Axis | Values |
|---|---|
| pairs | 1, 2, 4, 8 within the physical cores |
| mix | GET 100%; GET/PUT 99/1, 95/5, 90/10 |
| distribution | uniform, Zipf, hot key; same shard and distributed shards |
| working set | L2, LLC, 4× LLC |
| value | 64 B, 1 KiB, 64 KiB, 256 KiB |
| pipeline | 1, 8, 32, 128 |
| connections | 1 and 4 per pair; then a saturation sweep |
| durability | volatile, sync, group, periodic |
| writer batch | 1, 4, 16, 32, 128 |
| overlap | burst, queue full, rotation, incremental compaction |

Read-after-write has a dedicated cell: for the same connection and pair, every PUT ACK is followed
by a GET and must return exactly the version just acknowledged.

## Required metrics

- throughput and latency p50/p95/p99/p99.9/max;
- admission→dequeue, dequeue→publication, publication→durability and durability→completion;
- depth/high-water/full of the two SPSC queues;
- records and bytes per batch/publication;
- CPU per Reader/Writer, IPC, branch miss, cache/LLC/dTLB miss;
- context switch, CPU migration, syscall and wakeup;
- copied bytes, allocations in the timed path and componentized RSS;
- live/retired generations, retire backlog and delay;
- time and number of sockets with read interest disarmed for backpressure.

On Linux, `perf stat` and `perf record` are collected separating the Reader and Writer PID/TID. On
macOS, Instruments Time Profiler/Counters and affinity/QoS declared as advisory are used. No
unavailable counter is estimated from throughput.

## Gate per phase

### Volatile prototype, one pair

- zero mutex/queue/allocation on the GET, verified with instrumentation;
- at most two lookups;
- correct read-after-write and queue wraparound;
- memory unchanged after saturation/drain;
- GET p99 under 95/5 not worse than the baseline beyond the interleaved noise.

### Multi-pair

- GET efficiency of at least 80% from 1→the number of available physical pairs;
- no cross-pair traffic after bind;
- no CPU migration in the hard-pinned Linux baseline;
- a slow pair does not materially alter the p99 of the other pairs.

### Durability

- outcome and recovery equivalent to the v1 suite;
- no sync/group ACK before the durable boundary;
- queueing, publication and commit delay reported separately;
- complete crash/fault matrix for append→publish→durable→completion.

### Compaction and QSBR

- no Reader wait for copy/commit;
- retire backlog and memory stay bounded during slow socket output;
- p99/p99.9 with overlap compared, not only throughput;
- no reclamation before the quiescence demonstrated by TSAN and deterministic fault gates.

## Experiments not to confuse with the baseline

- Reader/Writer on SMT siblings;
- oversubscription;
- PGO/native CPU only on the paired runtime;
- client and server running concurrently on the same cores;
- tmpfs versus a real filesystem;
- internal GET versus TCP;
- different durability policy or batch.

These runs are useful as sensitivity analysis, but do not support an architectural claim.

## Required results

The final report must show absolute values, paired/current ratio, min/median/max, dispersion and the
complete environment. An outcome is `pass`, `fail` or `inconclusive`; values within the combined
spread are `inconclusive`. Success requires correctness, boundedness and tail latency together, not
a higher mean ops/s.

## Outcome of the first gate and backlog

The isolated GET data path passes the preliminary gate: 2.75× at 64 B and 6.11× at 1 KiB, with a
lower p99. The prototype as a whole instead fails the 95/5 throughput: 0.30× and 0.25× relative to
the current runtime. The primary cause is the cumulative copy of the delta and of the payloads
during every publication.

Binding order before the Reactor integration:

1. P0 — stable payload storage and compact generation-pinned `RecordView` — completed in the
   prototype with a shared `StableRecord` outside the GET;
2. P0 — delta without cumulative copy — completed with immutable copy-on-write pages;
3. P0 — exact allocator/copy instrumentation — ingress copies, directory ownership, raw views,
   pages and logical records are measured. General-purpose PMR and a single slab were rejected by
   the gates; the production allocator hooks must be designed per size class and bounded retention;
4. P1 — experimental Reactor with socket ownership and QSBR output lease;
5. P1 — multi-pair 1/2/4/8, affinity/topology and isolation between pairs;
6. P1 — TCP pipeline 1/8/32/128 and values 64 B–256 KiB with equivalent semantics;
7. P2 — Segment v1, sync/group/periodic durability, crash/fault/recovery;
8. P2 — incremental compaction and reclamation with a slow socket.

The second gate satisfies the condition for item 4: 95/5 at 2.15× the baseline at 64 B and 4.57× at
1 KiB, with GET p99 of 208 and 208 ns respectively against 292 and 500 ns. The result remains
micro/in-process and does not yet authorize default activation in the daemon.

## Outcome of the single-pair Reactor TCP gate

The wire-to-wire target uses the public C++ client, a single pair, 1–8 connections, a deterministic
workload and interleaved A/B order. Debug, ASan+UBSan and TSan all pass the 446 tests. The slow
output path is covered by forcing a partial write and verifying the pin and the release of the
generation.

Outcome per area:

| Area | Outcome | Main evidence |
|---|---|---|
| GET 64 B, pipeline 32 | preliminary throughput pass | +2.0%/+4.8%/+0.6%/+1.6% with 1/2/4/8 connections; p99 not uniform |
| GET 64 B, pipeline 1/8 | inconclusive | -1.8%/-2.5%, within a band to confirm on an isolated host |
| GET 64 B, pipeline 128 | preliminary pass | +11.4%, lower batch p99 |
| GET 1 KiB–256 KiB | preliminary pass | +21% up to +233%; scatter/gather dominates on large values |
| mixed 64 B 99/1 | fail | -18%, batch p99 +38%, mean publication batch 1.07 |
| mixed 64 B 95/5 | critical fail | -54%, batch p99 +131%, mean publication batch 1.25 |
| mixed 64 B 90/10 | critical fail | -73%, batch p99 +262%, mean publication batch 1.43 |
| bounded output/reclamation | pass | pin only on partial write, bounded high-water, zero final backlog |

The latencies reported by the TCP target are p50/p99/p99.9 of the completion of the entire client
batch. They cannot be presented as the p99 of the GET alone when the batch contains a mutation. The
next benchmark must separate read/write histograms and read-only/write-only connections.

For now the gate forbids multi-pair and daemon integration. The new P0 is the cost of the isolated
mutation: every connection correctly admits a single mutation before the read-after-write barrier,
so with four clients the Writer forms mean batches too small to repay the handoff, immutable
publication and completion wakeup. Raising only `max_wait` brings the 95/5 batch up to 2.52 records
at 32 us, but reduces throughput; it is not a solution.

P0 order:

1. profile separately submit→dequeue, build delta, publication and completion→ACK;
2. adaptive spin/park of the Reader after submit and Reader→Writer wakeup, distinct per profile;
3. reduce allocations and cache-line traffic of the isolated publication with bounded retention;
4. allow coalescing of consecutive PUTs of the same connection up to the first GET barrier, without
   early ACK or visibility;
5. repeat 99/1 and 95/5 at 64 B; authorize multi-pair only if 99/1 falls within the noise and the
   isolated GET p99 improves under a concurrent Writer.
