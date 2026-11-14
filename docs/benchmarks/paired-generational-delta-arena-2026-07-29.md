# Paired generational Delta arena gate

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Accepted with a P1 performance follow-up.** The Delta no longer owns a `shared_ptr<ReadRecord>`
control block for each published version. Immutable records are 64 B cells in Writer-owned
append-only blocks; Swiss pages and generations copy only stable pointers. Long keys and
Segment/file generation pins live in the arena, which stays owned by the Delta state until QSBR
quiescence.

The gate is positive for safety, boundedness, memory and volatile GET. It is not a universal win:
PUT→GET loses about 1.8% of throughput and durable-periodic GET about 1.5% on this unpinned macOS
host. This residual cost must be profiled on the publication path and verified on hard-pinned Linux;
it is not to be attributed to the owner-bound GET, which improves.

## Layout and invariants

- `DeltaRecord` is trivially copyable, aligned and exactly one cache line (64 B);
- record blocks of 64 cells and key blocks of 4 KiB never move published memory;
- the Writer builds the record, key and pins before making the pointer reachable from the new
  generation; release publication and acquire adoption are the only cross-thread edge;
- the Reader never consults mutable vectors, cursors or counters of the arena;
- during the merge a state owns at most the cut arena and the post-cut arena; the same post-cut cell
  is referenced by the cumulative delta and by the post-cut delta, without double materialization;
- the capacity counts **versions**, not only logical keys. Repeated churn on the same key starts the
  merge at the threshold and applies backpressure before exhausting the arena;
- volatile and durable pins are deduplicated per generation; the fast path compares the last pin
  first, which is the normal case for the active Segment;
- reclamation of cells, keys and pins coincides with the retirement of the generation after
  quiescence.

The per-pair metrics expose `delta_entries`, `delta_record_versions`, allocated record bytes, used
key bytes and key block capacity. This makes overwrite amplification and slack visible.

## Method

- macOS arm64, Apple LLVM 21, Release, one shard pair and 8 owner-bound clients;
- key 16 B, value 64 B;
- GET pipeline 32: 4 warm-ups and 7 samples of 50,000 operations;
- PUT→GET pipeline 1: 2 warm-ups and 5 samples of 100,000 pairs;
- same wire protocol, routing, batch and durability policy;
- RSS of the whole process; macOS affinity advisory, no hard pin.

The pre/post raw results and the confirmation runs are in
`benchmarks/results/paired-generational-delta-arena-2026-07-29/`.

## A/B results

| scenario | pre throughput | post | delta | pre RSS | post | delta |
|---|---:|---:|---:|---:|---:|---:|
| volatile GET p32 | 2.100 Mops/s | 2.171 Mops/s | **+3.4%** | 47.56 MB | 45.58 MB | **−4.2%** |
| volatile PUT→GET p1 | 214.2 kops/s | 210.3 kops/s | **−1.8%** | 138.23 MB | 134.63 MB | **−2.6%** |
| durable-periodic GET p32 | 187.2 kops/s | 184.4 kops/s | **−1.5%** | 76.53 MB | 74.84 MB | **−2.2%** |

| scenario | p50 pre/post | p95 pre/post | p99 pre/post | p99.9 pre/post |
|---|---:|---:|---:|---:|
| volatile GET p32 | 121.1 / 116.2 µs | 141.4 / 123.9 µs | 162.5 / 133.7 µs | 191.2 / 161.3 µs |
| volatile PUT→GET p1 | 71.8 / 72.7 µs | 101.2 / 102.8 µs | 123.3 / 126.1 µs | 425.5 / 167.0 µs |
| durable-periodic GET p32 | 875.8 / 919.0 µs | 1.593 / 1.611 ms | 1.960 / 1.986 ms | 2.284 / 2.296 ms |

The baseline PUT→GET p99.9 contains an outlier and does not support an improvement on its own. Two
post confirmation runs produced 209.8–210.3 kops/s: the small mixed regression is repeatable. The
earlier post durable run (185.4 kops/s) also confirms a band of −1% to −1.5%, not a collapse.

## Verification

- unit test of 65 overwrites on the same key plus an external key, with an old snapshot still
  readable and exact block accounting;
- Writer integration test: merge for distinct entries and merge triggered by versions with a single
  logical key, arena metrics returning to zero and a correct final value;
- Release suite: 467/467;
- Debug, ASan+UBSan, TSan and strict are part of the block's final gate.

## Decision and follow-up

The third layout differs from the two rejected candidates: it does not put inline records in the COW
pages and does not create a block per publication. It amortizes the metadata over 64 versions, keeps
addresses stable and explicitly bounds overwrite churn. It is therefore promoted despite the
measured mixed cost: it closes per-entry ownership and amplification, reduces RSS and improves the
read path.

The follow-up must not change lifetimes or add a Delta chain. It must be measured with a Writer
profile separating `prepare`, pin retention, arena store, page COW, publication and completion. The
next gate is hard-pinned Linux with `perf`; an optimization is acceptable only if it preserves
capacity per version, atomic publication and QSBR.

## Follow-up (2026-07-31)

Structural candidate: directory-chunk COW
([`paired-delta-directory-chunks-2026-07-31.md`](paired-delta-directory-chunks-2026-07-31.md)). The
magnitude of the mixed recovery remains to be confirmed on hard-pinned Linux.
