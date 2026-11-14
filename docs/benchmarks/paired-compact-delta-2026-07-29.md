# Paired Delta layout gate

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Rejected (historical gate).** The copy-on-write Delta kept `shared_ptr<const ReadRecord>` handles
per entry. Two compact layouts preserved correctness, but neither improved throughput, tail latency
and memory at the same time compared with the current layout. The experimental code did not stay in
the engine.

## Method

- macOS arm64, Apple LLVM 21, Release build;
- one `ShardPair`, 8 owner-bound clients, pipeline 32;
- key 16 B, value 64 B;
- volatile and durable-periodic GET: 4 warm-ups + 7 samples of 50,000 operations;
- volatile PUT→GET: 2 warm-ups + 5 samples of 100,000 operations;
- comparison against the compact Base Index already accepted;
- RSS refers to the whole benchmark process.

The raw results are in `benchmarks/results/paired-compact-delta-2026-07-29/`.

## Candidates

1. **Inline records in the COW page.** Removes the per-entry control block, but every page copy
   duplicates 16 complete records. Compared with the baseline, RSS grows by about 24% in volatile
   GET, 25% in durable and 21% in mixed. Rejected for structural amplification.
2. **Immutable block per publication batch.** Pages hold raw pointers and the Delta state owns the
   blocks. The first variant reserved 32 records even when the publication held one: RSS +112%
   volatile and +66% durable. A second variant used exact capacity and adaptive GC, removing that
   waste, but did not pass the performance gate.

## Best variant against baseline

| scenario | baseline throughput | candidate | delta | baseline RSS | candidate | delta |
|---|---:|---:|---:|---:|---:|---:|
| volatile GET | 2.128 Mops/s | 1.824 Mops/s | **−14.3%** | 45.84 MB | 48.09 MB | **+4.9%** |
| durable-periodic GET | 186.4 kops/s | 189.7 kops/s | +1.8% | 74.29 MB | 73.35 MB | −1.3% |
| volatile PUT→GET | 362.9 kops/s | 342.9 kops/s | **−5.5%** | 61.29 MB | 64.06 MB | **+4.5%** |

In volatile GET the candidate worsens p50/p95/p99/p99.9 by about 13.7%/33.5%/37.3%/28.5%
respectively. In mixed it improves latencies, but loses throughput and memory. In durable the
throughput is nearly neutral while p99.9 worsens by about 12.8%. The trade-off is not acceptable for
a Reader-path optimization.

## Technical conclusion

The real workload often publishes small batches. A per-batch arena introduces lifetime metadata,
block retention and an indirection without guaranteeing density; the inline record in the page makes
the COW granularity too expensive. The per-entry handle remains, for now, the best measured
compromise.

The removal was later closed with a third, substantially different layout: an append-only arena per
generation, compact cells separate from the pins, and a bounded capacity on versions. The decision
and the A/B are in
[`paired-generational-delta-arena-2026-07-29.md`](paired-generational-delta-arena-2026-07-29.md).
