# Paired Writer deadline/fairness validation — macOS arm64 — 2026-08-27

Status: local engineering evidence, not a release or production-readiness gate.

Revision: `5feccd3-dirty`. The worktree also contains the preceding maintenance,
pre-intent compaction, backup-fence and strict-group-sync corrections; these runs
must not be interpreted as an isolated causal comparison.

## Correctness scope

- deterministic fault-injection litmus proves that a dedicated Writer gives an
  already-admitted async mutation a turn after the first 32 records of a
  caller-owned 64-item sync batch;
- durable-group integration test sets `group-max-wait=1500 ms` and
  `async-queue-wait=20 ms`, observes `OVERLOADED` before 750 ms, and proves after
  restart that the expired mutation never entered Store;
- native debug suite: 650/650 passed;
- focused ASan and TSan runs passed for both new litmus tests and the strict
  durable-group commit-slot synchronization regression.

## Targeted throughput guard (`async_queue_wait_ms=0`)

Raw-wire RAW workload, 800 PUT + 800 GET frames, one shard pair, four clients,
pipeline 32, 64-byte values, one warmup and five measured repetitions.

| Group records | Throughput | p99 | Physical commit mean | Paired collection mean | Deadline closes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 441.713 ops/s | 674.092 ms | 4.458 ms | 70 ns | 0 |
| 4 | 1,697.75 ops/s | 166.021 ms | 4.503 ms | 25.634 µs | 0 |

Against the immediately preceding strict-sync-fix measurements in
`../local-macos-2026-08-27-strict-group-sync-fix/`, throughput moved +0.6% at
batch 1 and +5.0% at batch 4. P99 moved +1.2% and +3.8%, respectively; the run is
short, its min/max spread is material, and macOS affinity is advisory, so these
movements are treated as host variance rather than performance claims. The
important guard is that normal TCP-only
durable-group traffic records zero queue-deadline and sync-fairness closes while
retaining the prior throughput envelope.

## Commands

```text
glifistore_server_benchmarks --ops 800 --workers 1 --clients 4 --pipeline 32 \
  --executor-affinity --latency --workload read-after-write \
  --storage-mode durable-group --group-max-records {1,4} \
  --group-max-bytes 1048576 --group-max-wait-ms 10 --warmup 1 --repeats 5
```

The deterministic fairness litmus uses one 64-item `Store::put_batch`, proves at
least one 32-record turn split, and observes the already-admitted async completion
between Writer turns. Multi-hour adversarial and non-macOS evidence remain open.
