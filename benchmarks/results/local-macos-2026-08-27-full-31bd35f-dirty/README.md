# Full local benchmark campaign — 2026-08-27

Campaign run from the dirty pre-commit tree at `31bd35f` on an Apple M4 with the
`macos-native-release` build (`Release`, native CPU on, LTO off). This is local
performance evidence, not a CI baseline, a cross-platform comparison, or a
durability certification.

> **Partial invalidation (2026-08-27):** the batch-1 and batch-4 TCP `durable_group` cells with four
> clients reached a Writer threshold that was incorrectly closed as deferred. Those raw files and
> their aggregate rows are not valid strict durability performance evidence. Corrected measurements
> and the exact scope are in `../local-macos-2026-08-27-strict-group-sync-fix/`. Other cells are not
> retroactively promoted to durability certification.

## Coverage

- 222 successful commands: the canonical 219-command matrix plus three generation-publication diagnostics;
- core Store and Index, owner-bound/uniform/Zipf scaling at 1/2/4/8 workers;
- TCP volatile, pipeline 1/8/32/128, 1/2/4/8 pairs and mixed workloads;
- sync/group/periodic durability, embedded and TCP paths;
- experimental paired Reactor A/B, compaction, maintenance, rotation, churn and idle;
- 165 standard-format source files aggregated into 180 strict report results;
- custom TSV/CSV suites retained verbatim beside the standard report.

The exact invocation list is in `commands.txt`, the machine record in
`environment.txt`, and the strict aggregate in `results.md` / `results.json`.

## Selected absolute results

| Workload | Median throughput | p50 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: |
| Store GET copy, 64 B | 3.76 Mops/s | — | — | — |
| Store PUT, 64 B | 547.86 kops/s | — | — | — |
| Owner-bound GET copy, 1 worker | 3.67 Mops/s | — | — | — |
| Owner-bound GET copy, 2 workers | 8.22 Mops/s | — | — | — |
| Owner-bound GET copy, 4 workers | 17.32 Mops/s | — | — | — |
| Owner-bound GET copy, 8 workers | 23.97 Mops/s | — | — | — |
| TCP volatile GET, 1 pair, pipeline 32 | 894.91 kops/s | 34.54 us | 41.46 us | 52.21 us |
| TCP volatile GET, 4 pairs, pipeline 32 | 2.62 Mops/s | 44.04 us | 88.67 us | 108.04 us |
| TCP volatile GET, 8 pairs, pipeline 32 | 3.67 Mops/s | 66.25 us | 97.58 us | 117.50 us |
| TCP sync read-after-write, 1 pair, pipeline 1 | 421.53 ops/s | 4.92 ms | 8.02 ms | 14.86 ms |
| TCP group read-after-write, 1 pair, pipeline 1 | 422.26 ops/s | 4.98 ms | 11.61 ms | 15.62 ms |
| TCP periodic read-after-write, 1 pair, pipeline 1 | 10.56 kops/s | 74.29 us | 5.16 ms | 6.90 ms |

## Paired and publication findings

The experimental paired Reactor A/B is neutral-to-positive on median pure-GET
throughput at pipeline 32 (+2.50% with one client, +0.99% with four), but its
sampled p99 batch latency is worse (+43.23% and +4.95%). With 10% PUT it is not
competitive yet: median throughput is lower by 35.82% with one client and
38.66% with four; sampled p99 is worse by 7.20% and 35.69%. The measurements
also show publication batches of one record in the one-client case and only
1.68 records on average with four clients. Publication/batching remains the
highest-priority performance problem; pure GET throughput alone is not a
success criterion.

The direct bounded slot publication candidate improves the two-thread protocol:

| Generation diagnostic | Shared ownership | Direct slot | Delta | Publication p99 |
| --- | ---: | ---: | ---: | ---: |
| adoption only | 1.101 Mpub/s | 1.177 Mpub/s | +6.91% | effectively flat (1541 vs 1542 ns) |
| adoption plus GET | 818.21 kpub/s | 937.43 kpub/s | +14.57% | -11.40% (1834 to 1625 ns) |

GET sampling in the second diagnostic is unchanged at 83 ns p50 and 250 ns
p99. macOS affinity calls were unavailable, so these are scheduler-exposed local
results rather than pinned-core evidence.

## Benchmark-discovered backpressure

The first `maintenance/mixed-disabled` attempt correctly exposed a harness
assumption: bounded pre-Store generation admission can return
`resource_exhausted` while concurrent embedded readers have not reached a
quiescent point. The old harness counted that known-not-committed outcome as a
lost PUT. It now retries only the two normative generation-admission outcomes,
with a five-second bound, includes the wait in PUT latency, and reports
`generation_backpressure_retries`.

All seven corrected repeats completed every PUT. Three repeats observed
backpressure (834, 276 and 223 retries); four observed none. Cooperative and
background maintenance repeats observed none. The original failed output is
retained as `maintenance/mixed-disabled-first-failure.txt` rather than discarded.

## Interpretation limits

- There is no environment-compatible retained baseline for this native build, so the strict report suppresses historical deltas.
- Same-host client/server placement, APFS temporary stores, scheduler movement and thermal state can affect absolute values.
- These results do not close any assurance or production-readiness gate.
