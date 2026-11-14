# Full local benchmark campaign — 2026-08-30

Campaign run from clean `main` at
`b971a157b420f8a850730ac6137560c1614c4dc7` on the same Apple M4 host and
`macos-native-release` configuration used by the
[2026-08-29 `1ff35c3` campaign](../local-macos-2026-08-29-full-1ff35c3/README.md).
This is local performance evidence, not a CI baseline, a cross-platform comparison,
a production-capacity claim, or durability certification.

## Integrity and coverage

- 222/222 commands completed (`ok=222`, `fail=0`, `skip=0`);
- the command matrix is byte-for-byte identical after replacing only the output directory;
- Release, native CPU on, LTO off, Apple clang 21.0.0, macOS 26.6.2, APFS, AC power;
- core Store and Index; owner-bound/uniform/Zipf scaling at 1/2/4/8 Workers;
- volatile TCP at pipeline 1/8/32/128, paired Reactor, sync/group/periodic durability;
- compaction, maintenance, forced rotation, churn, idle, and generation diagnostics;
- 222 raw source files in `results.json`; 180 canonical results matched the compatible baseline.

Exact invocations are in `commands.txt`, environment identity in `environment.txt`, raw output in
the workload subdirectories, and the generic aggregate in `results.md` / `results.json`.
`runner_image_version` remains the 2026-08-29 local-image identity because the host image, kernel,
compiler, build preset, CPU, and benchmark contract did not change; the actual run timestamp and OS
build are recorded separately.

The aggregate report is intentionally not described as strict. The current strict parser rejects
the specialized compaction/maintenance formats because they do not repeat the common
`arch`/`platform`/`compiler`/sampling metadata inside every raw file. Their harness-level validation
passed, but this metadata gap must be closed before the complete 222-file local campaign can become
strict retained evidence.

## Canonical comparison summary

The automated range-overlap policy classified the 180 matched canonical results as:

| Classification | Count |
| --- | ---: |
| Improvement candidate | 14 |
| Regression candidate | 12 |
| Inconclusive because min/max ranges overlap | 154 |

A candidate is not a confirmed change. The benchmark standard requires repetition across runs;
single-campaign medians are particularly sensitive to scheduler, filesystem, and thermal noise.

### Embedded 64-byte paths

| Workload | Current median | Delta vs `1ff35c3` | Range classification |
| --- | ---: | ---: | --- |
| Store GET copy | 4.25 Mops/s | +30.16% | improvement candidate |
| Store PUT | 427.84 kops/s | +6.26% | inconclusive |
| Store PUT batch | 673.51 kops/s | +11.00% | improvement candidate |
| Store PUT + GET | 814.22 kops/s | +14.07% | improvement candidate |
| Store read-after-write | 822.40 kops/s | +6.56% | inconclusive |

Owner-bound GET copy reaches 4.06 / 9.27 / 18.98 / 23.39 Mops/s at 1/2/4/8 Workers.
All four comparisons overlap the baseline ranges. Eight-Worker throughput is 5.76x the one-Worker
result (72.0% scaling efficiency); this remains a same-host result on a ten-core heterogeneous CPU.

Owner-bound PUT reaches 0.49 / 0.70 / 1.12 / 1.93 Mops/s. The four-Worker point is a -8.11%
regression candidate, while 1/2/8 Workers overlap the baseline. The isolated one-Worker Zipf PUT
cell is also a regression candidate at -23.50%.

### Volatile TCP, GET-only, pipeline 32

| Reader–Writer pairs | Current median | Delta | p50 | p99 | Classification |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 884.98 kops/s | +4.12% | 34.71 us | 77.42 us | inconclusive |
| 2 | 1.66 Mops/s | +3.96% | 36.67 us | 52.33 us | improvement candidate |
| 4 | 2.71 Mops/s | +6.18% | 42.04 us | 88.17 us | improvement candidate |
| 8 | 3.65 Mops/s | +0.42% | 69.92 us | 455.54 us | inconclusive |

At eight pairs, throughput is 4.12x the one-pair result (51.6% efficiency). The eight-pair p99 rose
from 101.79 us to 455.54 us despite stable median throughput. Because server and clients share ten
logical cores and macOS affinity is advisory, this is a tail-latency warning rather than an isolated
engine attribution.

The strongest canonical overload regression is volatile read-after-write at eight pairs and
pipeline 128: 195.72 kops/s, -42.93%, with p99 rising from 6.78 ms to 35.21 ms and p99.9 from
8.32 ms to 72.93 ms. Its throughput ranges are disjoint, so this cell needs a focused repeat before
the change can be accepted.

The focused repeat was performed after the campaign analysis, using the identical command and an
interleaved same-host A/B against a detached `1ff35c3` worktree. Across three seven-sample rounds,
the current-tree median-of-medians was 301.19 kops/s versus 287.44 kops/s for `1ff35c3` (+4.78%);
the corresponding p99 median-of-medians was 7.29 ms versus 7.45 ms (-2.15%). Individual rounds
varied widely: current 252.33–304.61 kops/s and baseline 262.59–312.50 kops/s. Relative to the
negative campaign cell, the focused current result is +53.88% throughput with 79.29% lower p99.
The original -42.93% cell is therefore not reproduced as a code regression; it remains evidence of
same-host scheduler/thermal variance under advisory affinity and deep pipeline pressure.

### Durability samples

These rows have different acknowledgement semantics and must not be compared as equivalent modes.

| Mode, 1 pair / pipeline 1 / read-after-write | Median | Delta | p50 | p99 | Classification |
| --- | ---: | ---: | ---: | ---: | --- |
| v1 sync | 351.05 ops/s | -19.44% | 4.77 ms | 19.56 ms | inconclusive |
| strict group | 358.03 ops/s | -8.53% | 5.10 ms | 11.63 ms | regression candidate |
| periodic | 10.25 kops/s | -4.67% | 73.75 us | 4.85 ms | inconclusive |

With four clients on one Writer at pipeline 32, strict group commit peaks in this sweep at
1.41 kops/s with `group_max_records=4`, roughly 4.1x the batch-1 result. Its p50 response latency is
96.80 ms, so batching raises throughput by keeping more work in flight; it does not remove the
durable acknowledgement cost.

## Specialized diagnostics

The embedded paired-shard experiment reports 13.20 Mops/s for borrowed-span GET versus 6.04 Mops/s
for the current owning-copy Store path, and 10.92 versus 3.92 Mops/s for its 95/5 mixed workload.
Those are deliberately different ownership semantics and are not an official-runtime A/B claim.

In the paired Reactor at four clients and pipeline 32, the paired/current throughput ratio is 1.02x
for pure reads, then 0.86x / 0.76x / 0.66x at 1% / 5% / 10% PUT. Across the complete 32-cell matrix,
paired wins 13 cells. The experimental paired transport therefore has no general performance win
under write pressure even though both implementations improved materially against the previous
campaign in several read-heavy cells.

High-reclaim compaction improved from 186.98 ms to 164.95 ms (-11.78%) with disjoint ranges.
Copy-heavy compaction improved at the median from 873.70 ms to 712.07 ms (-18.50%), but ranges
overlap. Other compaction median changes are between -4.85% and +0.86% in elapsed time.

Generation-shell microbenchmarks improved by 6.84–11.20%, with disjoint ranges for all six
implementations. Concurrent publication moved in the opposite direction at the median:
adoption-only is down 5.63–7.31% and publication with reader GET is down 16.30–17.54%; every one of
those publication ranges overlaps because the new samples include substantial scheduler outliers.

Maintenance is noisy. Mixed-background throughput is -6.68% and p99 is +97.90%; churn-background
throughput is -7.67% and p99 is +117.07%. All corresponding ranges overlap. Cooperative mixed
throughput improves +22.58% at the median while its p99 falls 18.42%, also with overlapping ranges.
These are prioritization signals only, not confirmed regressions or improvements.

## Current assessment

The read path is healthy and materially faster in several canonical cells, while GET scaling remains
strong through four Workers and continues to increase at eight. The write path is mixed: low and
moderate concurrency are mostly stable, but Zipf PUT, four-Worker owner-bound PUT, and especially
eight-pair/deep-pipeline read-after-write need controlled repetition. Durable sync/group operation
remains dominated by millisecond-scale persistence and queueing; group batching trades latency and
in-flight memory for throughput.

The results do not close any production-readiness or durability gate. In particular, the benchmark
campaign does not override the CI failures observed for `b971a15`. The macOS concurrency litmus,
clang-tidy optional-access diagnostics, and an additional Erlang fake-server registration race have
subsequently been repaired and verified locally where the required runtime is available; the
follow-up still requires a fresh green CI run. The complete local-report metadata gap is a separate
assurance issue.
