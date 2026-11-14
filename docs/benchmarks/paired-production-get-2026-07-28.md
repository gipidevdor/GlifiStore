# Paired production GET A/B audit

**Historical** · macOS arm64, AppleClang 21, Release · 2026-07-28

Local comparison between `94aae2d` and the paired migration worktree. One warmup and three samples;
median; 16-byte keys, 64-byte values. The raw summary is in
`benchmark-results/paired-shards/94aae2d-dirty/macos-arm64/summary.csv` (a local directory ignored
by Git by policy). Pipeline 32 measures the progressive completion of the batch, not the isolated
lookup.

| Workload | Topology | Δ throughput | Δ p50 GET | Δ p95 GET | Δ p99 GET | Δ p99.9 GET | Δ RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| GET-only | 4 pair / 8 clients | +3.65% | -2.90% | -1.84% | -1.23% | -7.35% | +36.6% |
| GET-only | 1 pair / 8 clients | -1.90% | -0.74% | +14.81% | +12.34% | +37.18% | +36.8% |
| 99% GET / 1% PUT | 1 pair / 8 clients | -13.90% | -7.77% | +24.14% | -4.66% | -0.92% | +36.5% |
| PUT→GET, pipeline 1 | 1 pair / 1 client | -51.80% | +88.76% | +97.34% | +71.05% | +59.86% | +29.6% |

Assessment: the owner-bound GET is now genuinely mutex-free and scales positively with four pairs,
but the overall gate is not closed. The double handoff strongly penalizes synchronous mutations; the
two-level structure penalizes some single-pair cases; the duplicated handles and keys increase
memory too much.

During the A/B a first publication that rebuilt the delta on every ACK was discarded: in 99/1 it
produced a GET p99.9 on the order of 14 ms. The version measured in the table uses copy-on-write
Swiss pages, a hierarchical directory and a bounded QSBR; it brought the 99/1 p99.9 back to ~320 µs,
on par with the baseline.

Next P0 items:

1. incremental, non-monolithic delta→base merge;
2. compact `ImmutableReadIndex` without a `shared_ptr` per entry or a double copy of the keys;
3. preallocated mutation slot pool;
4. publication/reclaim telemetry and Reader/Writer benchmark on separate connections;
5. scatter/gather/get-into;
6. durable generation with file pins and bootstrap recovery.
