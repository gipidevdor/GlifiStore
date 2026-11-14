# Incremental delta→base merge

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

The incremental merge removes the Writer's main head-of-line blocking when the delta reaches the
threshold. In the volatile PUT→GET load with 50,000 unique keys, hence six crossings of the
8,192-entry threshold, p99 drops from 89.5 ms to 2.77 ms and the maximum service time from 187.8 ms
to 1.98 ms. Throughput grows by 164%. The observed cost is about +6.6% of median RSS during the
merge load.

The gate does not yet authorize a production-ready statement: the central PUT→GET latency rises by
about 5%, durable-periodic GET shows a noisy p99.9, and the memory still holds duplicated
handles/keys that must be compacted in the next P0.

## Environment and method

- Apple arm64, macOS, Apple LLVM 21.0.0;
- Release build, no hard affinity available (`cpu_pin_applied=0`);
- raw TCP protocol, one shard pair, 8 owner-bound clients, pipeline 32;
- 2 warmups and 5 samples; the reported values are medians, except the explicit maxima;
- baseline: detached worktree of commit `a65e454`, with the monolithic merge;
- candidate: working tree of the same commit with the incremental merge and the previous durable
  blocks;
- the old/new runs are not interleaved; small variations must therefore be treated as indicative.

The complete raw output is in `benchmarks/results/paired-incremental-merge-2026-07-29/`.

## A/B — volatile PUT→GET across the merge

50,000 logical operations generate 100,000 measured frames. The keys are unique: the delta crosses
the threshold six times and does not measure only updates of entries already present.

| Metric | Monolithic | Incremental | Delta |
|---|---:|---:|---:|
| throughput | 110,467 frame/s | 291,718 frame/s | +164.1% |
| time/frame | 9,052.5 ns | 3,428.0 ns | −62.1% |
| p50 | 897.3 µs | 945.0 µs | +5.3% |
| p95 | 1.852 ms | 1.948 ms | +5.2% |
| p99 | 89.48 ms | 2.775 ms | −96.9% (32.3×) |
| p99.9 | 187.49 ms | 4.249 ms | −97.7% (44.1×) |
| median queue wait | 88.91 µs | 8.32 µs | −90.6% |
| maximum queue wait | 187.78 ms | 1.835 ms | −99.0% (102.4×) |
| median service | 18.90 µs | 5.12 µs | −72.9% |
| maximum service | 187.78 ms | 1.984 ms | −98.9% (94.6×) |
| median RSS | 58.90 MB | 62.80 MB | +6.6% |
| peak RSS | 63.47 MB | 67.35 MB | +6.1% |

The slightly worse p50/p95 is not hidden: the double update of the delta during the post-cut and the
layout still based on `shared_ptr` have a central cost. The important result is the removal of the
tail pause of about 188 ms; the next compact layout must also recover the central cost without
reopening the tail.

## Steady-state GET gate

| Mode | Metric | Before | After | Delta |
|---|---|---:|---:|---:|
| volatile | throughput | 1.952 Mops/s | 2.248 Mops/s | +15.2% |
| volatile | p50 | 121.5 µs | 109.3 µs | −10.0% |
| volatile | p95 | 145.0 µs | 125.0 µs | −13.8% |
| volatile | p99 | 183.0 µs | 178.8 µs | −2.3% |
| volatile | p99.9 | 232.0 µs | 271.8 µs | +17.2% |
| durable-periodic | throughput | 179,534 ops/s | 179,528 ops/s | neutral |
| durable-periodic | p50 | 948.7 µs | 926.5 µs | −2.3% |
| durable-periodic | p95 | 1.731 ms | 1.754 ms | +1.3% |
| durable-periodic | p99 | 2.152 ms | 2.192 ms | +1.9% |
| durable-periodic | p99.9 | 2.799 ms | 3.429 ms | +22.5% |

The volatile sample closes the no-regression gate of the normal GET at p99, but not at p99.9.
Durable-periodic is neutral in throughput and mixed in the tails; the dispersion between samples and
the lack of affinity prevent attributing the p99.9 to the layout with enough confidence.

## Verified invariants

- Reader: at most two lookups (`D` then `B`), no mutex and no queue in the owner-bound GET;
- Writer: a single mutator, builder and merge cursors entirely thread-local;
- publication: a single release/acquire pointer to an immutable generation;
- post-cut: every mutation updates the visible cumulative delta and only the private `Dpost`;
- work: a quantum counts at most 4,096 slots, including empty ones;
- memory: delta hard cap 40,960 entries; post-cut default 32,736; no unbounded growth;
- backpressure: capacity verified before the Store mutation;
- refresh: rotation/compaction can replace the generation and cancel the merge while keeping the
  pins;
- recovery: no persistent byte or v1 authority was modified.

## Validation

- Debug: 458 tests, 0 failures;
- ASan + UBSan: 458 tests, 0 failures;
- TSan: 458 tests, 0 failures;
- allocation fault injection: pass;
- crash matrix sync/periodic/group and daemon: 8/8 pass;
- whitespace diff: clean.

## Decision

The incremental merge is promoted into the production paired runtime. The old monolithic merge
remains only in the test/oracle API and is not called by `PairWriterPool`. The next P0 is the
compact read-only layout: remove one `shared_ptr` per entry and the duplicated keys, measuring
base/delta lookup and memory during the double generation separately.
