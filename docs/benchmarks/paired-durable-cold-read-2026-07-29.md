# Paired durable cold-read audit

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Scope

Audit on macOS arm64, AppleClang 21, Release, raw-wire protocol, owner-bound routing, 16 B keys and
64 B values. The comparison starts from the durable-periodic path preceding the SPSC cold-read
commit and ends at worktree `a65e454` with two further optimizations:

- one I/O helper and one private SPSC per `ShardPair`, without a global mutex/condition variable;
- a reusable, verified record scratch, consumer-private per lane;
- cancellation through a preallocated epoch per connection slot, without a `shared_ptr` or an
  allocation per GET.

The local raw summaries are in
`benchmark-results/paired-shards/a65e454-dirty/macos-arm64/2026-07-29-durable-cold-read/`.
Because macOS has no hard affinity and the runs are not alternated, variations below about 5% are
indicative, not conclusive.

## Results

| Workload | Topology | Before | After | Δ throughput | p50 before/after | p99 before/after | p99.9 before/after |
|---|---:|---:|---:|---:|---:|---:|---:|
| GET-only, pipeline 32 | 1 pair / 8 clients | 162,351 | 179,973 ops/s | +10.9% | 961 / 924 µs | 2.30 / 2.06 ms | 3.07 / 2.41 ms |
| GET-only, pipeline 32 | 4 pair / 8 clients | 159,791 | 159,108 ops/s | -0.4% | 885 / 864 µs | 2.19 / 2.06 ms | 2.93 / 3.49 ms |
| GET-only, pipeline 1 | 1 pair / 8 clients | 99,209 | 94,677 ops/s | -4.6% | 82 / 78 µs | 185 / 156 µs | 261 / 212 µs |
| 99% GET / 1% PUT | 1 pair / 8 clients | 143,796 | 167,718 ops/s | +16.6% | 797 / 809 µs | 4.08 / 3.89 ms | 5.49 / 5.37 ms |

The reusable scratch, isolated before the epoch cancellation, took the 1-pair GET-only from 168,320
to 178,890 ops/s (+6.3%) and reduced p95/p99/p99.9. The epoch removes another allocation and its
refcount, with a smaller gain but simpler, bounded ownership.

## Assessment

The block closes the local single-pair cold GET regression and improves the mixed workload without
moving I/O under a lock. It does not close the durable scalability gate: at 32 clients and
pipeline 32, one pair produces 180,484 ops/s and four pairs 176,309 ops/s. The same volatile harness
reaches 3.76 Mops/s at four pairs, so the limit is specific to the durable materialization path, not
to the TCP reactor alone.

The current profile still performs, for every cold GET:

1. a copy of the `PublishedReadRecord` with an increment of the `shared_ptr` pin;
2. an allocation of the `PreparedColdRead` pImpl;
3. a `pread` and a full Record/CRC verification;
4. an allocation and copy of the value into `OwnedValue`;
5. an I/O→Reader wakeup and a copy into the socket output queue.

The single I/O lane per pair pays a handoff/wakeup for every record. Adding threads to the same pair
would violate SPSC/ownership and mask the problem; the right direction is to reduce
materializations, copies and syscalls, then measure batching/io_uring on Linux while preserving
ordering.

## Safety gate

- full CTest: 37/37, including crash and allocation fault matrices;
- Debug suite: 453/453;
- ASan/UBSan: 453/453;
- TSan: 453/453;
- late completion/connection-slot reuse and shutdown with in-flight pins remain covered;
- the new scratch has an explicit test of capacity reuse and value validation.

## Next priorities

1. **P0 — cold-read lease:** remove the pImpl and the per-request refcount with preallocated slots
   and a QSBR that covers I/O and response, without letting `RecordRef` or unpinned file handles
   escape.
2. **P0 — get-into/scatter-gather:** a single bounded materialization of the value and an explicit
   response lifetime; TLS requires a separate path because it does not automatically preserve
   external iovecs.
3. **P1 — platform backend:** measure `io_uring` on Linux and completion batching; macOS keeps the
   portable backend until a native backend shows a better tail.
4. **P1 — observability:** per-lane counters (submit, wake, batch, service, scratch growth, bytes
   copied) and an alternating 1/2/4/8 pair benchmark with real CPU/NUMA affinity on Linux.

The P0 refresh is closed by the per-shard protocol documented in the ADR and by its A/B audit in
`paired-generation-refresh-2026-07-29.md`.

Update 2026-07-29: items 1 and 2 were closed by the gates
`paired-borrowed-cold-read-2026-07-29.md` and `paired-scatter-output-2026-07-29.md` respectively;
scatter is adaptive, while TLS and pipelined connections stay contiguous by contract and by
benchmark.

Update 2026-08-01 (paired adoption): item 3 stays **deferred** for 0.1.0 — no A/B shows a better
tail or fewer syscalls without risk to ordering/ack
([`paired-shards-plan.md`](paired-shards-plan.md)). Item 4 is covered by the hard-pinned Linux
harness awaiting `glifistore-linux-perf` ([`paired-shards-linux-p1.md`](paired-shards-linux-p1.md)).
