# Paired durable generation refresh audit

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Goal

Rotation and compaction replace the runtime catalog pins. The previous paired `ReadGeneration` was
correct because it owned the exact descriptors, but it held them until restart. This block adds
refresh and reclamation without moving I/O or index construction onto the Reader.

## Verified protocol

- monotonic revision **per shard**, incremented after the new runtime catalog is installed;
- coherent snapshot `Index + exact generation pins + revision` under the Worker/catalog lock,
  without I/O;
- construction of the immutable base on the Writer and release publication of the single generation
  pointer;
- monotonic `visible_through` even when the highest sequence belongs to a deleted key;
- old generation in a bounded retire list, freed after acquire adoption and Reader quiescence;
- reclamation wakeup even without new mutations;
- non-destructive retry on `resource_exhausted`, fail-closed on inconsistencies;
- no cross-shard refresh: a compaction of pair 0 leaves pair 1 unchanged.

The metrics `read_catalog_revision`, `read_refresh_attempts/successes/failures/deferrals`,
`generations_retired` and `retired_generation_count` are available per lane through `STATS`.

## Steady-state A/B

macOS arm64, AppleClang 21, Release, durable-periodic, 64 B, 1 pair, 8 clients, pipeline 32, 50,000
operations, 2 warmups and 5 samples. The "before" binary ran immediately before the rebuild of the
same worktree; macOS applies no hard affinity.

| Variant | Throughput | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| before the revision poll | 180,710 ops/s | 900 µs | 1.636 ms | 2.009 ms | 2.384 ms |
| per-shard refresh | 179,476 ops/s | 917 µs | 1.653 ms | 2.020 ms | 2.431 ms |
| delta | -0.68% | +1.89% | +1.05% | +0.55% | +1.94% |

The difference is within the expected operational noise and is not a demonstrated regression. The
additional 4 pair / 32 client run produced 170,849 ops/s; it confirms that the scalability of
durable cold materialization remains the bottleneck already identified, and does not prove an effect
of the refresh because an alternating A/B with affinity is missing.

## Tests and limits

- Debug: 453/453;
- ASan/UBSan: 453/453;
- TSan: 453/453;
- end-to-end test: rotation → compaction → `RecordRef` change → refresh → adoption → retirement;
- multi-pair test: revision and rebuild confined to the compacted shard.

Today the refresh rebuilds the whole base on the Writer. It does not block GET, but it can raise the
PUT latency of the shard with large datasets. The next architectural work on publication is
therefore the incremental/compacted immutable index merge; it must not move onto the Reader and must
not be solved with an unbounded chain of deltas.
