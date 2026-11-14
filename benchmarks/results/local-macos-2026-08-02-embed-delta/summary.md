# Lab: embed DeltaState in PairReadGeneration (2026-08-02)

Claim ceiling: Apple M4 / `macos-release` lab evidence only.

## Change

`DeltaState` is owned by value inside the `make_shared` generation helper
(`PairReadGenerationEnableShared`), so each publish co-allocates generation shell +
delta in one heap block. No TLS freelist, no custom deleter. Publish / lease /
RAW protocol unchanged. Writer sync path also scopes `GS_PHASE_PUT(worker_apply)`
and `GS_PHASE_PUT(publish)` for lab attribution when phases are compiled in.

## Medians (200k ops, warmup 3, repeats 11)

| Cell | Run1 | Run2 | Run3 | Prior plain* | Prior make_shared gen* |
| --- | ---: | ---: | ---: | ---: | ---: |
| `store_put` 1t | 378 k | 372 k | 370 k | 344 k | 362 k |
| `store_put_batch` 1t | 556 k | — | — | 515 k | 536 k |
| affine PUT 2t | 460 k | 415 k | 454 k | 503 k† | 432–463‡ |
| uniform PUT 2t | 119 k | 131 k | 134 k | 130 k | 129 k |
| `store_get_copy` 1t | 4.03 M | — | — | — | 3.63 M |

\* Same-day earlier sessions, not interleaved with this binary.  
† Earlier plain affine looked high; interleaved make_shared A/B placed affine in
~436–463 k — treat † as thermal/order, not a hard gate.  
‡ Interleaved make_shared vs plain affine A/B from earlier in the day.

## Verdict

**Keep.** Directional single-op / batch PUT gain without freelist-style affine
cliff. Absolute PUT still publish/apply bound (≪ 600 k target). Next leverage is
Writer apply slim + phase dumps with `GLIFISTORE_HOT_PATH_PHASES`, not another
TLS recycle pool.
