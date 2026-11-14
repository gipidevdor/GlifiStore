# Lab: make_shared PairReadGeneration (2026-08-02)

Claim ceiling: Apple M4 / `macos-release` lab evidence only.

## Change

Publish path uses `std::make_shared` via a private `EnableShared` helper so the
generation object and `shared_ptr` control block share one heap allocation.
No freelist, no custom deleter. Protocol unchanged.

## Interleaved affine A/B (authoritative for parallel)

Same filter `store-parallel-put` worker-affine 2t, 200k ops, warmup 3, repeats 11,
back-to-back rebuilds:

| Order | Variant | Median kops/s |
| --- | --- | ---: |
| 1 | make_shared | 437 |
| 2 | plain `new` + `shared_ptr` | 436 |
| 3 | make_shared | 463 |

**Verdict:** neutral vs plain on affine (earlier −14% vs a colder baseline was
order/thermal noise). Safe to keep.

## Same-session cells vs earlier plain baseline

| Cell | make_shared | Earlier plain | Note |
| --- | ---: | ---: | --- |
| `store_put` 1t | 362 k | 344 k | mild gain; not gate |
| `store_put_batch` 1t | 536 k | 515 k | mild gain |
| uniform PUT 2t | 129 k | 130 k | neutral |
| `store_get_copy` 1t | 3.63 M | — | no regress vs program ~3.5 M |

## Related reject

TLS shell freelist (ADR 0035) remains rejected; see
`../local-macos-2026-08-02-gen-shell/summary.md`.
