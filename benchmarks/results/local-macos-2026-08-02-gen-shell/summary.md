# Lab: generation shell freelist A/B (2026-08-02)

Claim ceiling: Apple M4 / `macos-release` lab evidence only.

## Verdict

**Reject** TLS `PairReadGeneration` shell freelist + custom `shared_ptr` deleter
(ADR 0035). Mild single-thread PUT gains do not offset a clear worker-affine
parallel PUT regression.

## Method

- Same binary family, same machine, back-to-back runs
- 200 000 ops, key=16, value=64, warmup=3, repeats=11
- Candidate: freelist (capacity 64) — `local-macos-2026-08-02-gen-shell/`
- Baseline: plain `new` + default deleter — `...-gen-shell-ab-baseline/`

## Medians

| Cell | Freelist | Baseline | Δ |
| --- | ---: | ---: | ---: |
| `store_put` 1t | 359 k | 344 k | +4.4% |
| `store_put_batch` 1t | 540 k | 515 k | +4.9% |
| uniform PUT 2t | 131 k | 129 k | +1.0% |
| worker-affine PUT 2t | 422 k | 503 k | **−16.2%** |

GET sanity on freelist candidate: `store_get_copy` ~4.07 M (no regress vs prior ~3.5 M program result; noise/machine).

## Note on absolute levels

Uniform 2t ~130 k in this session is below the earlier program cell (~241 k). The
A/B comparison above is the authoritative reject signal; absolute levels vary with
thermal/load and are not used alone to accept/reject.
