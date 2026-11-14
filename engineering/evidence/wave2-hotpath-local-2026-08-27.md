# Wave 2 hot-path residual — local evidence

- **Date:** 2026-08-27
- **Branch:** `debt/l1l5-wave2-hotpath`
- **Evidence class:** `local` (see `docs/assurance/evidence-taxonomy.md`)
- **Claim ceiling:** architectural prototype

## What landed

| Surface | Change | Proof |
| --- | --- | --- |
| `lane_state.hpp` | Pad `queued_bytes`; Writer telemetry barrier; `LaneMetrics` `alignas(128)` | static_assert + build |
| `adopt_read_generation` | Skip `reader_safe_epoch` acq_rel when unchanged | memory-order inventory |
| Writer / sync waiter | pause → yield → park ladder | combining + paired tests |
| GET ≤64 B | Zero-heap steady-state forbid-alloc | `glifistore_allocation_fault_tests` |
| Combining | durable_sync × token FIFO; put_batch RAW; hot-key sibling shard | `shard_combining_executor_tests` |
| SPSC | Hot-producer backpressure; slow-consumer fairness | `bounded_mpsc_queue_tests` |

## Explicit non-goals / residuals

- ADR 0036 remains **proposed**; default `generation_slot_pool` stays **false**.
- No dual TCP ports / io_uring / get-into.
- durable_sync combiner still one publish per committed mutation (multi-op generation coalesce deferred).
- Delta COW `make_shared` page clones unchanged (prior freelist rejected; needs measured win).
- Absolute GET/PUT p99 budgets remain Wave 6 / hardware.
- macOS unpinned benches are **not** scaling claims.

## Conflict with L1 slot pool

Wave 2 does not own publish/reclaim APIs. Dual-path `publish_read_generation_token`
(relaxed pointer + release token) left intact. Layout padding is orthogonal to slot-pool
storage.
