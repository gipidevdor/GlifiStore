# Encode precomputed-size + apply sub-phase attribution

Date: 2026-08-02 (lab, Apple Silicon, `macos-release`)
Claim ceiling: architectural prototype / same-machine lab evidence only.

## Change

- `encode_record(out, input, encoded_size)` reuses a size already computed by
  `encoded_record_size` (Segment append + durable encode scratch). Same bytes;
  debug builds assert size agreement; release keeps input validation + extent
  consistency checks.
- Lab-only phases: `encode_copy` (Segment append encode) and `index_publish`
  (Worker::publish).

## Cells (phases OFF)

| Cell | Median |
| --- | ---: |
| `store_put` 1t | 363 k ops/s |
| worker-affine PUT 2t | 517 k ops/s |
| `store_get_copy` 1t | 3.56 M ops/s |

Prior same-day deque-pin cell: PUT ~377 k / affine ~504 k — encode-size delta is
within noise; expected given encode is ~66 ns/op.

## Attribution (phases ON, `store-put` 200k × 3)

| Phase | mean_ns | pct of PUT section |
| --- | ---: | ---: |
| ack | 2698 | 64.1% |
| publish | 603 | 14.3% |
| worker_apply (outer) | 514 | 12.2% |
| index_publish | 158 | 3.8% |
| enqueue | 100 | 2.4% |
| encode_copy | 66 | 1.6% |
| admit | 34 | 1.6% |

`encode_copy` + `index_publish` nest inside `worker_apply`; percentages are not
disjoint. Residual remains wake/schedule (`ack`), then generation publish.
