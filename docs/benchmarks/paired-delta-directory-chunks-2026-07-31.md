# Paired Delta directory-chunk COW

**Historical** · macOS arm64, advisory affinity, Release · 2026-07-31

## Outcome

**Accepted as a bounded structural optimization; magnitude mixed, not closed on macOS.** The P1
follow-up to the gate
[`paired-generational-delta-arena-2026-07-29.md`](paired-generational-delta-arena-2026-07-29.md)
reduces the publication cost without weakening the QSBR lifetime or the capacity counted in
**versions**.

The spine of the Delta directory at maximum capacity (~40,960 entries) exposed ~256
`shared_ptr<DeltaDirectoryBlock>` copied on every `DeltaBuilder`. Every PUT→GET at pipeline 1 paid
that refcount traffic even for a single record. The chunked layout introduces a second COW level
(`DeltaDirectoryChunk`, 16 blocks per chunk): the publication copies ~16 chunk pointers and performs
COW only on the touched chunk, block and page.

Side micro-optimizations, same lifetime:

- O(1) counters for arena key bytes (no scan of the key blocks on every stats update);
- in-place construction of the 64 B `DeltaRecord` cell;
- last-page cache on the builder for multi-mutation batches.

## Preserved invariants

- 64 B arena cells, blocks of 64 records, long keys in 4 KiB blocks;
- capacity counted in versions; merge/backpressure unchanged;
- publication release / Reader acquire as the only cross-thread edge;
- reclamation only after QSBR quiescence;
- no additional Delta chain, no `shared_ptr<ReadRecord>` handle per version.

## A/B method (advisory macOS)

- same arm64 host, Apple LLVM 21, Release;
- baseline = tree `fddb355` without this patch; candidate = patch;
- interleaved order `old/new/new/old` (+ a confirmation cycle);
- volatile PUT→GET p1: key 16 B, value 64 B, 1 pair, 8 clients, warmup 2, repeats 5, 100,000 ops;
- volatile GET p32 as a control;
- advisory Mach affinity; **not** hard-pinned.

Raw: `benchmark-results/paired-shards/fddb355-dirty/macos-arm64/2026-07-31-p1-delta-directory-chunks/`.

## Results

The thermal/load spread on the same host is wide (PUT→GET samples from ~60 k to ~208 k ops/s). The
full recovery of the historical −1.8% against the pre-arena layout is not claimed.

| subset | old median | new median | note |
|---|---:|---:|---|
| all interleaved+confirm PUT→GET samples | ~122 k | ~149 k | spread dominates; no claim |
| samples ≥150 k ops/s (less throttled) | ~179 k | ~205 k | directional, still advisory |
| volatile GET p32 | 1.998 M | 1.967 M | ~−1.6%, within local noise |

No production-ready claim on mean throughput. No `e3_certified=yes`.

## Verification

- Debug `glifistore_tests`: 471/471 on the candidate tree;
- arena overwrite/version capacity tests unchanged (`pair_read_generation_tests`);
- lifetime and merge path not restructured beyond the directory spine.

## Decision and follow-up

The chunked directory COW is promoted because it removes a structural tax on single-record
publication without touching capacity or QSBR. The residual P1 on **magnitude** (how much of the
historical mixed −1.8% is recovered) stays open until the hard-pinned Linux A/B
([`paired-shards-linux-p1.md`](paired-shards-linux-p1.md)).
