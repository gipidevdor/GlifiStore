# Correctness follow-up benchmark campaign — 2026-08-30

Campaign run from source commit
`9ba73973dc67b92dd9b33ca7f0eb726469c161b9` on the same Apple M4 host and
`macos-native-release` configuration as the
[`b971a15` campaign](../local-macos-2026-08-30-full-b971a15/README.md) and the
[`1ff35c3` campaign](../local-macos-2026-08-29-full-1ff35c3/README.md).
This is local performance evidence, not a CI baseline, a cross-platform comparison, a
production-capacity claim, or durability certification.

## Integrity and coverage

- 222/222 serialized commands completed (`ok=222`, `fail=0`, `skip=0`), with no non-empty stderr
  files;
- 222 raw benchmark files were aggregated; 180 canonical results matched each compatible baseline;
- the command matrix is identical to `b971a15` after replacing only the output directory;
- the environment identity is identical for `9ba7397`, `b971a15`, and `1ff35c3`;
- Release, native CPU on, LTO off, Apple clang 21.0.0, macOS 26.6.2, APFS, AC power;
- Store, Index, 1/2/4/8-Worker scaling, volatile TCP, sync/group/periodic durability, compaction,
  maintenance, churn, and generation diagnostics are present.

Exact invocations are in [`commands.txt`](commands.txt), environment identity in
[`environment.txt`](environment.txt), raw output in the workload subdirectories, and the immediate
aggregate in [`results.md`](results.md) / [`results.json`](results.json). The historical comparison
against `1ff35c3` is retained separately in
[`results-vs-1ff35c3.md`](results-vs-1ff35c3.md) /
[`results-vs-1ff35c3.json`](results-vs-1ff35c3.json).

As in the preceding campaign, the full aggregate is not labelled strict: specialized compaction,
maintenance, and generation formats do not repeat the common metadata in every row. Their harness
checks passed. The benchmark harness and schema-v7 report parser now close this gap for future runs;
these historical raw files remain exploratory because evidence must not be rewritten retroactively.

### Post-campaign structured diagnostic reanalysis

The schema-v7 parser was also applied read-only to the retained specialized CSV/TSV files. It
recovered 108 direction-aware diagnostics and matched all 108 against each environment-compatible
baseline. Because the historical files predate the common metadata fix, these counts are diagnostic
reanalysis rather than strict evidence.

| Baseline | Improvement candidate | Regression candidate | Overlapping ranges |
| --- | ---: | ---: | ---: |
| `b971a15` | 1 | 10 | 97 |
| `1ff35c3` | 7 | 4 | 97 |

Against `b971a15`, five of the ten candidates are the common 4.5–7.6% generation-shell shift;
three are paired-Reactor cells below 2%, one is churn-disabled (-17.56%), and one is the already
focused high-reclaim compaction result. Against `1ff35c3`, those generation-shell and churn ranges
overlap; the four candidates instead are direct publication/adopt (-8.39%), forced rotation
(+116.82% duration), one paired-Reactor cell (-2.14%), and one paired-shard cell (-2.93%). This
turnover reinforces the existing assessment: the broad negative counts are dominated by host,
scheduler, and I/O variability, while the retained raw phase metrics remain available for focused
reproduction.

## Immediate comparison with `b971a15`

The range-overlap policy classified the 180 canonical results as:

| Classification | Count |
| --- | ---: |
| Improvement candidate | 6 |
| Regression candidate | 3 |
| Inconclusive because ranges overlap | 171 |

This is the relevant comparison for the correctness changes. The 3 initial regression candidates
were rerun in three interleaved A/B rounds against binaries built from the exact baseline commit.

| Candidate | Full campaign delta | Focused current | Focused baseline | Focused delta | Outcome |
| --- | ---: | ---: | ---: | ---: | --- |
| Owner-bound read-after-write, 2 Workers | -15.98% | 1.314 Mops/s | 1.355 Mops/s | -3.00% | not reproduced; round deltas -3.00% / +10.32% / -5.29% |
| Periodic TCP GET, 4 pairs, pipeline 32 | -23.41% | 94.83 kops/s | 99.46 kops/s | -4.66% | not reproduced; round deltas -0.31% / -4.66% / +0.68% |
| Embedded periodic read-after-write | -31.39% | 14.46 kops/s | 15.08 kops/s | -4.14% | not reproduced; round deltas -4.14% / -2.32% / +4.93% |

The periodic TCP focused p99 is 2.13 ms on `9ba7397` versus 3.80 ms on `b971a15`, despite the small
throughput delta. The embedded round-three slowdown affected both revisions in the same phase. No
canonical candidate therefore supports a reproducible regression caused by the correctness patch.
Raw focused results and exact ordering are in [`focused-ab/`](focused-ab/); the derived values are
also recorded in [`focused-comparisons.json`](focused-comparisons.json).

The six immediate improvement candidates are Index find-hit (+7.49%) and replace (+55.44%),
one-Worker Zipf PUT (+32.36%), eight-Worker owner-bound read-after-write (+3.55%), embedded durable
group GET (+26.60%), and one-pair periodic TCP read-after-write (+72.96%). They remain candidates,
not performance claims.

## Same historical baseline as the preceding campaign

Using `1ff35c3` again makes the candidate counts directly comparable with the preceding report:

| Campaign against `1ff35c3` | Improvement | Regression | Overlap |
| --- | ---: | ---: | ---: |
| `b971a15` | 14 | 12 | 154 |
| `9ba7397` | 11 | 7 | 162 |

Nine of the previous 12 regression candidates move back into the overlap class: Index insert and
erase, one-Worker Zipf PUT, four-Worker owner-bound read-after-write, the volatile two-pair 99/1
cell, the prior eight-pair/deep-pipeline outlier, embedded durable-group GET, group p1
read-after-write, and sync p32 99/1. Three persist: embedded sync recovery-open (-42.10%), group p8
read-after-write (-12.69%), and four-Worker owner-bound PUT (-8.51%). Four different cells become
single-campaign candidates: group p8 GET (-41.13%), embedded sync GET (-40.06%), and volatile
read-after-write at one/four pairs with pipeline 32 (-6.18% / -29.92%).

All seven historical candidates overlap `b971a15` in the immediate comparison. In particular, the
persistent group p8 read-after-write is +7.88% versus `b971a15`, embedded sync GET is +6.74%, and
four-Worker PUT is -0.44%. The reduction from 12 to 7 against the same historical baseline is useful
evidence, but the turnover of noisy durability/TCP cells means it is not a causal claim that five
specific optimizations removed five regressions.

## Current throughput and latency profile

The embedded 64-byte steady-state paths remain stable versus `b971a15`: GET 4.26 Mops/s (+0.23%),
PUT 433.22 kops/s (+1.26%), batch PUT 671.71 kops/s (-0.27%), PUT+GET 779.29 kops/s (-4.29%), and
read-after-write 808.78 kops/s (-1.66%). Every range overlaps.

Owner-bound GET reaches 4.41 / 9.96 / 19.33 / 25.46 Mops/s at 1/2/4/8 Workers. Eight-Worker
throughput is 5.77x the one-Worker result (72.2% scaling efficiency). Owner-bound PUT reaches
0.49 / 0.68 / 1.12 / 1.97 Mops/s; all four immediate comparisons overlap. This is important for the
new coherent global-routing publication: the benchmark does not show a reproducible hot-path cost.

Volatile TCP GET at pipeline 32 reaches 0.89 / 1.64 / 2.61 / 3.65 Mops/s. The eight-pair p99 is
99.5 us, down from the preceding campaign's 455.5 us while throughput is effectively unchanged.
At one pair/pipeline 1, durable read-after-write is 411 ops/s for sync, 361 ops/s for strict group,
and 10.14 kops/s for periodic; all three immediate comparisons overlap. Group batching with four
clients peaks at 1.57 kops/s at batch 32 in this sweep, with a 89.85 ms p50, preserving the expected
throughput/latency tradeoff.

## Specialized diagnostics

The full-run high-reclaim compaction median is 193.27 ms versus 164.95 ms at `b971a15`. A focused
three-round A/B did not give a stable code attribution: round one was equal (-0.92% elapsed), while
rounds two and three were +59.02% and +9.49%. The slower current rounds also had a 1.9x/1.1x slower
independent seed phase, before measured compaction. Median-of-medians is still 187.09 ms versus
128.94 ms, so this remains an unresolved host-I/O/thermal risk rather than being discarded. Raw data
is retained in [`focused-compaction-ab/`](focused-compaction-ab/).

Generation-shell medians are 4.5–7.6% below `b971a15`, while publication with adoption is mixed
(shared +6.83%, direct -2.93%) and publication with Reader GET is +10.96% / +14.66%. Maintenance
remains scheduler-sensitive: mixed cooperative improves throughput 47.28% with p99 down 55.73%,
while mixed background is -14.08% with p99 up 18.88%; churn throughput is 11.44–17.56% lower across
modes. These specialized ranges contain substantial outliers and are prioritization signals only.

## Assessment

The correctness campaign did not introduce a reproducible canonical performance regression. The
core 64-byte paths are stable, read scaling is strong through four Workers and still increases at
eight, the earlier deep-pipeline tail outlier is materially smaller, and five fewer historical
regression candidates remain against the same `1ff35c3` baseline.

The residual performance concerns are durable sync/recovery variability, group p8 queueing,
four-Worker PUT versus the older baseline, noisy write-heavy TCP cells, compaction sensitivity to
host I/O state, and maintenance/churn tail latency. Dedicated-host repetition and phase-level
instrumentation are more appropriate next actions than weakening the correctness guards on the
basis of overlapping local samples.

These results do not close a production-readiness or durability gate. GlifiStore remains an
architectural prototype.
