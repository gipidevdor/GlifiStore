# Full local benchmark campaign — 2026-08-29

Campaign run from clean `main` at `1ff35c3` on Apple Silicon with the
`macos-native-release` build (`Release`, native CPU on, LTO off). This is local
performance evidence, not a CI baseline, a cross-platform comparison, or a
durability certification.

## Coverage

- 222/222 commands completed (`ok=218` newly measured + `skip=4` resumed from a
  short prior partial run; `fail=0`);
- core Store and Index, owner-bound/uniform/Zipf scaling at 1/2/4/8 workers;
- TCP volatile, pipeline 1/8/32/128, multi-pair and mixed workloads;
- sync/group/periodic durability, embedded and TCP paths;
- experimental paired Reactor A/B, compaction, maintenance, rotation, churn;
- 222 raw source files aggregated into the generic report (`results.md` /
  `results.json`). The complete suite is not strict-parser evidence because the
  specialized compaction/maintenance formats do not repeat all common metadata
  inside each raw file.

Exact invocations are in `commands.txt`, machine record in `environment.txt`.

## Selected absolute results

| Workload | Median throughput | p50 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: |
| Store GET copy, 64 B | 3.26 Mops/s | — | — | — |
| Store PUT, 64 B | 402.62 kops/s | — | — | — |
| Owner-bound GET copy, 1 worker | 4.17 Mops/s | — | — | — |
| Owner-bound GET copy, 2 workers | 9.30 Mops/s | — | — | — |
| Owner-bound GET copy, 4 workers | 16.31 Mops/s | — | — | — |
| Owner-bound GET copy, 8 workers | 23.17 Mops/s | — | — | — |
| TCP volatile GET, 1 pair, pipeline 32 | 849.93 kops/s | 35.17 us | 77.08 us | 125.38 us |
| TCP volatile GET, 4 pairs, pipeline 32 | 2.55 Mops/s | 43.75 us | 91.79 us | 113.50 us |
| TCP volatile GET, 8 pairs, pipeline 32 | 3.63 Mops/s | 67.17 us | 101.79 us | 137.67 us |
| TCP sync read-after-write, 1 pair, pipeline 1 | 435.75 ops/s | 4.81 ms | 6.46 ms | 10.73 ms |
| TCP group read-after-write, 1 pair, pipeline 1 | 391.40 ops/s | 5.00 ms | 7.61 ms | 12.00 ms |

## Notes

- Sampling defaults to warmup 1 / repeats 7 unless a harness file states otherwise
  (generation diagnostics use warmup 3 / repeats 11).
- macOS affinity calls remain advisory/unavailable; client and server share the
  local host/process.
- Do not treat these medians as production capacity or as E3/E4 evidence.
