# Compact Base Index

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

The Reader's Base Index no longer keeps a `shared_ptr`, a `std::string` and an owning pin per entry.
Each read-only record takes at most 64 bytes, keeps keys of up to 16 bytes inline and uses a block
arena for long keys. Segment and file generation pins are deduplicated per generation. Control
bytes, hashes and slots are flat in the published layout; the Writer initializes the control bytes
in quanta before copying Base and Delta, so it does not reintroduce a monolithic pause.

The final result improves throughput and memory in durable and PUT→GET workloads. On pure volatile
GET the median throughput stays about 3% below the baseline, while p95–p99.9 and RSS improve
strongly. In PUT→GET, p95–p99.9 are about 3–4% worse in the final sample, despite better throughput
and RSS. The block is therefore promoted as a bounded, memory-safe foundation, but it does not yet
close the Reader performance gate: interleaved A/B with real affinity and perf/CPU counters on Linux
are needed before declaring the Base final.

## Layout and invariants

- `CompactReadRecord <= 64 B`, verified by `static_assert`;
- keys `<= 16 B` inline; larger keys in stable blocks of at least 64 KiB;
- a single owning pin per distinct Segment/file generation, never one per record;
- stable entry pointers: `records_` reserves the maximum size before construction;
- three flat arrays for control bytes, hashes and record pointers in the published Base;
- memory reserved before the merge and control bytes initialized in bounded quanta;
- the Reader sees the arrays only after the release publish of the complete generation;
- Delta still paged COW and at most two lookups per GET (`Delta`, then `Base`);
- no change to Record, Segment, Manifest, routing or wire v1/v2.

## Environment and method

- macOS arm64, Apple LLVM 21, Release;
- one shard pair, 8 owner-bound clients, pipeline 32, raw-wire protocol;
- 50,000 keys of 16 B and values of 64 B;
- baseline: 2 warmups and 5 samples immediately before the compact layout;
- final candidate: 4 warmups and 7 samples for GET, 2 warmups and 5 samples for PUT→GET;
- `cpu_pin_applied=0`: on macOS the runs are not hard-pinned and small differences remain
  indicative.

The raw output, including the rejected intermediate layouts, is in
`benchmarks/results/paired-compact-read-index-2026-07-29/`.

## Final A/B

| Workload | Metric | Before | After | Delta |
|---|---|---:|---:|---:|
| volatile GET | throughput | 2.194 M/s | 2.128 M/s | −3.02% |
| volatile GET | median RSS | 48.43 MB | 45.84 MB | −5.35% |
| volatile GET | p50 | 111.9 µs | 118.0 µs | +5.44% |
| volatile GET | p95 | 209.8 µs | 127.0 µs | −39.48% |
| volatile GET | p99 | 290.2 µs | 135.1 µs | −53.45% |
| volatile GET | p99.9 | 425.4 µs | 163.3 µs | −61.61% |
| durable-periodic GET | throughput | 181,471/s | 186,431/s | +2.73% |
| durable-periodic GET | median RSS | 75.17 MB | 74.29 MB | −1.18% |
| durable-periodic GET | p50 | 903.9 µs | 876.8 µs | −3.00% |
| durable-periodic GET | p95 | 1.658 ms | 1.605 ms | −3.21% |
| durable-periodic GET | p99 | 2.042 ms | 1.969 ms | −3.58% |
| durable-periodic GET | p99.9 | 2.437 ms | 2.291 ms | −5.97% |
| volatile PUT→GET | throughput | 341,578 frame/s | 362,867 frame/s | +6.23% |
| volatile PUT→GET | median RSS | 62.31 MB | 61.29 MB | −1.63% |
| volatile PUT→GET | p50 | 866.8 µs | 866.5 µs | −0.03% |
| volatile PUT→GET | p95 | 1.760 ms | 1.832 ms | +4.05% |
| volatile PUT→GET | p99 | 2.407 ms | 2.505 ms | +4.06% |
| volatile PUT→GET | p99.9 | 3.557 ms | 3.673 ms | +3.23% |

## Designs rejected during the A/B

1. **Full contiguous `ReadRecord` in the Base.** It duplicated records across generations during the
   merge: volatile GET RSS 48.4→68.6 MB, durable 75.2→99.1 MB and PUT→GET 62.3→91.2 MB. Rejected.
2. **External arena for all keys.** It reduced memory but added an indirection to 16 B keys, with
   throughput regressions of up to about 10%. Rejected.
3. **Inline key and external pointer as separate fields.** It recovered the lookup, but pushed the
   record past the cache line and cancelled part of the RSS saving. Replaced by a 16 B union.
4. **Two-level paged directory.** It kept the merge lazy but paid two dependent loads per hit. A
   flat directory recovered only part of the gap. The final layout uses flat arrays with incremental
   initialization of the control bytes.
5. **Raw Segment pointer in the compact record.** It improved GET by less than 1% and required an
   ownership resolution per record during the merge. Rejected in favor of the pin index.

## Safety and quality

- Debug: 459 tests, 0 failures;
- allocation fault injection: pass;
- crash matrix sync/periodic/group and daemon: 8/8 pass;
- ASan + UBSan: 459 tests, 0 failures;
- TSan: 459 tests, 0 failures;
- strict ISO C++23/hardening build: pass;
- specific test: 16 B inline key, 17 B external boundary and key above 64 KiB;
- whitespace diff: clean.

## Decision and next P0

The compact Base stays in the paired runtime: it removes per-entry ownership, reduces amplification
and keeps the merge bounded without reopening Writer pauses. It is not labeled "performance
complete". The next work on the same axis is to remove the per-entry `shared_ptr<ReadRecord>` from
the **Delta** with a bounded Writer arena, then measure Base-hit, Delta-hit and miss separately with
`perf stat/record` (IPC, branches, LLC, dTLB and migrations) on hard-pinned Linux. Only that A/B can
close the residual −3% GET or justify a further layout.
