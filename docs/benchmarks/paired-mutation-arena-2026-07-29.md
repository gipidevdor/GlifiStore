# Paired mutation slot/key/value arena gate

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Pass for the P0 block mutation task storage.** The Reader no longer builds an owning `std::string`
or `std::vector` for `PUT`/`ERASE`: it copies the parsed bytes once into a preallocated bounded
arena of its own pair and transfers a fixed descriptor to the Writer through an SPSC queue. The slot
becomes reusable only after the acquire of the corresponding completion. The GET path was not
modified.

The unpinned macOS arm64 performance gate is positive on mutation-bound loads and essentially
neutral on the read-heavy control. It is not yet a Linux/NUMA production claim.

## Implemented protocol

Every lane owns:

- `capacity` fixed slots, equal to the capacity of the mutation/completion ring;
- a logical arena of `--durable-mutation-queue-bytes`;
- a physical guard at most as large as one frame, to keep contiguous a payload that crosses the wrap
  without performing a second copy;
- monotonic FIFO cursors and separate counters for payload and admission bytes;
- three observable rejection causes: slots exhausted, bytes exhausted and a single payload too
  large.

The linearization is:

```text
Reader copies payload into the slot
→ mutation queue release
→ Writer dequeue acquire and uses a borrowed view
→ completion queue release
→ Reader completion acquire
→ FIFO release and possible reuse of the slot
```

No `RecordRef`, span, file handle or other borrowed reference crosses the completion edge. An
out-of-order release, a completion without a valid slot or a payload no longer observable makes the
runtime fail-closed. A push failure immediately after the acquisition performs only the rollback of
the last unpublished slot.

With the current defaults a pair exposes 256 slots, 16 MiB of logical budget and about 18 MiB of
maximum virtual storage (budget plus frame guard). The memory is therefore explicit and bounded per
pair; the memory-constrained profile must reduce the byte budget, not introduce dynamic growth.

## A/B benchmark

Pre-arena binary kept before the Release recompilation, against the post-arena working tree. Apple
Silicon arm64, macOS, client and server on the same host, no hard affinity; 2 warmups and 7 samples.
The complete raw results are in
[`benchmarks/results/paired-mutation-arena-2026-07-29/`](../../benchmarks/results/paired-mutation-arena-2026-07-29/).

| Workload | Metric | Pre | Post | Delta |
|---|---:|---:|---:|---:|
| read-after-write, value 64 B, 8 clients, p1 | throughput | 178,278 ops/s | 212,638 ops/s | **+19.27%** |
| | p50 | 80.7 us | 72.3 us | **−10.38%** |
| | p95 | 128.6 us | 106.1 us | **−17.52%** |
| | p99 | 179.0 us | 138.8 us | **−22.47%** |
| | p99.9 | 435.0 us | 433.4 us | −0.36% |
| read-after-write, value 1 KiB, 8 clients, p1 | throughput | 203,662 ops/s | 207,187 ops/s | +1.73% |
| | p50 / p95 | 74.3 / 104.7 us | 73.7 / 103.5 us | −0.90% / −1.12% |
| | p99 / p99.9 | 129.1 / 428.0 us | 130.3 / 439.5 us | +0.90% / +2.69% |
| 99/1, value 64 B, 8 clients, p32 | throughput | 1.629 Mops/s | 1.589 Mops/s | −2.47% |
| | p99 / p99.9 | 234.1 / 323.4 us | 242.0 / 345.4 us | +3.40% / +6.82% |
| 99/1 post replica | throughput | — | 1.601 Mops/s | −1.70% vs pre |
| | p99 / p99.9 | — | 237.4 / 333.8 us | +1.42% / +3.21% vs pre |

The 64 B read-after-write improves even in the worst throughput sample (202,351 against 170,758
ops/s), so the gain does not depend on the median alone. At 1 KiB the intervals are narrow and the
advantage is small but repeatable. In 99/1 the pre/post spread overlaps and the second replica
reduces the regression: the outcome is **inconclusive/neutral**, to be rerun interleaved on
hard-pinned Linux. No GET improvement is claimed from this block.

The process median RSS grows by 2.5–6.2% in the post runs. The value includes the dataset, the
client/server processes of the same benchmark and the arena for every repeated instance. It is
consistent with the choice of turning a queue limit that was previously only accounting into
physically reservable storage; it is not acceptable to multiply it blindly by tens of pairs. The
next Linux gate must report RSS per component and resident working set after churn, in addition to
the virtual capacity already exposed by STATS.

## Correctness and robustness

- Debug: 466/466 tests;
- ASan + UBSan: 466/466 tests;
- TSan: 466/466 tests, including the cross-thread slot transfers;
- full ASan CTest: 37/37, including allocation-fault and crash matrices;
- `unix-strict` build: completed without new warnings;
- dedicated unit tests: stable copy, contiguous wrap, rollback, out-of-order release, distinct
  slot/byte/payload exhaustion;
- Reactor integration: saturation of the slot capacity and independent saturation of the byte
  budget, high-water and return to zero after completion.

## Decision and follow-up

The previous owning task storage is removed from the normal paired path. The closed P0 covers only
the mutation payload and descriptor; it does not automatically close the publication cost. The next
block remains the bounded Writer-owned Delta: a new candidate is promoted only if it beats the two
layouts already rejected without increasing copies, RSS or p99. In parallel, the production gate
still requires 1/2/4/8 hard-pinned Linux pairs, `perf` counters and memory per NUMA node.
