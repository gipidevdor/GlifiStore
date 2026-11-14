# Durable GET path SIMD hot-cache probe

**Historical** · durable GET path, local benchmark · 2026-07-25

## Scope

Continuation of the GET/hot-cache follow-up (`d493f63` code; prior medians in the removed
`get-path-hot-cache-followup-2026-07-24` notes). Hard constraints unchanged: exclusive Worker
mutex, authoritative per-Worker Index, GET linearization / pin identity, TTL, CRC, fail-closed,
no reader helpers / RCU / `shared_mutex` Index sharing / new executors / protocol changes.

## What changed

1. **Hot-cache SIMD control-byte probe** — `HotRecordTable` now uses Swiss-style H2 fingerprints in
   control bytes, 8-slot group probing, and the shared `swiss_control_group.hpp` SSE2/NEON/scalar
   matcher. Full key compare remains mandatory on fingerprint hits; hash is never identity.
2. **`prepare_get` catalog lock** — Ordinary hot path holds only the Worker mutex. Catalog shared
   lock is taken only for cold-miss generation pin / manifest identity. Cold revalidation takes the
   catalog lock only when the Index still names the captured `RecordRef`. TTL reclaim on cold expiry
   stays Worker-local.
3. **Unit tests** — `hot_record_table_tests` cover insert/replace/erase/tombstone reuse, multi-key
   probe chains, `erase_if`/`clear`, control matcher scalar equivalence, and reserve planning.

## How to reproduce

```bash
./scripts/dev.sh benchmark --filter store-durable-get --workers 1 --value-size 32 \
  --ops 5000 --warmup 1 --repeats 3
./scripts/dev.sh benchmark --filter store-durable-get --workers 1 --value-size 256 \
  --ops 5000 --warmup 1 --repeats 3
./scripts/dev.sh benchmark --filter store-durable-parallel-get --workers 4 --threads 4 \
  --value-size 256 --distribution zipf --ops 8000 --warmup 1 --repeats 3 --latency
```

Local raw outputs: gitignored `benchmark-results/get-path-simd/`.

Sanitizers: Debug and ASan `glifistore_tests` 373/0 after this change.

## Comparative medians (ops/s)

| Workload | Prior follow-up | This pass | Notes |
| --- | ---: | ---: | --- |
| durable GET w1 v32 | 2 358 140 @20 000 | **1 456 880** @5000 | ops-sensitive; @5000 still above prior @500 (1.35M) |
| durable GET w1 v256 | 1 248 540 @5000 | **1 336 420** @5000 | +7% |
| parallel GET w4 v256 zipf | 1 002 210 @8000 | **1 310 480** @8000 | +31% |
| parallel GET w4 v256 uniform | 1 277 440 @8000 | **2 150 950** @8000 | +68% (shorter catalog critical section) |
| durable GET w2 v131072 | 14 222 @200 | **15 468** @200 | size-rejected → cold |

Single-thread hot GET remains memory-bound (~0.7 µs/op at 5k ops). Parallel GETs benefit most from
deferring catalog shared-lock acquisition off the hot path.

## Still deferred / rejected

- Warm-from-cold GET admission (ADR 0017 write-admit policy).
- Worker-global generation version to skip cold revalidation (ABA).
- Global LRU / shared cache lock.
- Phase 5 abuse controls, E3/E4 power-loss, protocol changes.
