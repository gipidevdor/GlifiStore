# Strict paired group-commit synchronization correction

Date: 2026-08-27  
Host class: local Apple Silicon, macOS, APFS  
Build: native Release, dirty working tree at `5feccd3`  
Status: correctness-regression diagnostic; not release or cross-platform evidence

## Defect and correction

When a paired Writer batch reached the durable record or byte threshold,
`DurableRuntimeCatalog::mutate(..., writer_batch=true)` closed the Segment with
`SegmentCommitSync::deferred`. `StoreAccess::mutate_durable_batch()` then called
`commit_writer_batch()`, but the pending group was already empty, so that strict final commit was a
no-op. The path could return a successful `durable_group` completion without executing
`sync_commit_slot`.

The corrected rule is:

- a threshold reached inside a strict Writer batch closes with an immediate commit-slot sync;
- `commit_writer_batch()` immediately commits any residual group after the append loop;
- periodic/deferred mode retains its documented relaxed acknowledgement contract;
- the ACK and immutable-generation publication order is unchanged.

Two deterministic regressions cover the exact edge: the TCP two-record threshold test counts one
final commit-slot sync before responses, and a paired Store fault test injects failure at that sync
and requires both batch members to return `unavailable` with the runtime fail-closed.

## Reproduction

Each file was produced with the equivalent of:

```text
build/macos-native-release/glifistore_server_benchmarks \
  --ops 800 --workers 1 --clients 4 --pipeline 32 --executor-affinity --latency \
  --workload read-after-write --storage-mode durable-group \
  --group-max-records <1|4|16|32> --group-max-bytes 1048576 \
  --group-max-wait-ms 10 --warmup 1 --repeats 5
```

`operations=1600` is the combined 800 PUT + 800 GET protocol count. Throughput below is therefore
combined protocol throughput, not write-only throughput.

## Corrected medians

| Configured max records | Protocol ops/s | p50 | p99 | Batch records | Commit duration | Durable batches | Final sync contract |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 439.0 | 285.1 ms | 665.8 ms | 1 | 4.511 ms | 800 | immediate |
| 4 | 1,616.5 | 75.6 ms | 160.0 ms | 4 | 4.728 ms | 200 | immediate |
| 16 | 1,597.0 | 79.9 ms | 160.7 ms | 4 | 4.529 ms | 200 | immediate residual |
| 32 | 1,632.2 | 76.2 ms | 160.0 ms | 4 | 4.422 ms | 200 | immediate residual |

With four concurrent clients the observed useful occupancy is four. Raising the configured maximum
beyond four does not create more concurrency; it only leaves the four-record group to the explicit
residual commit. The measured APFS final synchronization dominates service time, as expected for a
strict durability contract.

## Invalidated earlier cells

The following retained raw cells reached the defective threshold and are **not valid strict
durability performance evidence**:

- `../local-macos-2026-08-27-full-31bd35f-dirty/durable/durable-group-w1-c4-p32-batch1.txt`;
- `../local-macos-2026-08-27-full-31bd35f-dirty/durable/durable-group-w1-c4-p32-batch4.txt`;
- `../local-macos-2026-08-27-completion-coalescing-ab/accepted-durable-group-batch4.txt`;
- the rejected `candidate-durable-quiet25-batch4.txt` experiment in that same folder.

The old batch-4 accepted diagnostic reported about 20,340 combined protocol ops/s and a 0.301 ms
“commit” duration because it measured the ordered Record barrier plus deferred slot publication,
not the required final slot synchronization. The corrected batch-4 row reports about 1,616 ops/s
and 4.728 ms. This is not a performance regression against a valid implementation; it is removal of
an invalid durability shortcut.

Batch-16 and batch-32 older runs did not hit their configured threshold and therefore reached the
explicit immediate residual commit. They are not listed as invalid, although scheduler occupancy and
the corrected sync pressure make their absolute timings unsuitable for a clean A/B comparison.

No production-readiness or physical power-loss gate is closed by this local APFS run.
