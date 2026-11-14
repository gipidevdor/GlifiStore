# ADR 0036 Wave 1 production slot pool — local evidence

- Date: 2026-08-27
- Updated: 2026-08-30 (V7 production-flag litmus and terminal merge-pin shutdown fix)
- Label: `local`
- Host: developer workstation (macOS)
- Status: Wave 1 opt-in path only; ADR remains **proposed**
- Claim ceiling: architectural prototype (no E3/E4, no production-ready)

## Scope closed locally

- Production `GenerationSlotPool` (capacity 65) with `{epoch:48, slot+1:16}` token
- `PairReadGeneration` direct constructors + merge-compatible direct publish
- Opt-in `PairedConcurrencyConfig::generation_slot_pool` (default **false**)
- Embedded combiner / dedicated Writer sync paths reserve-before-mutate
- Dedicated Writer async batch path reserves before Store and publishes via the pool
- Unit: V1 reincarnation (≥10k), V6 fail-closed, V9 exhaustion, token width
- Paired Store: V5 shutdown + V10 put_batch FIFO under the flag
- V7: embedded combiner and dedicated Writer publish incremental merge/post-cut state while a
  counted lease or daemon-style Reader frontier holds slot pressure
- Terminal close: after admission stop, Writer join, and Reader-lease validation, unfinished
  Writer-only merge state is discarded so its cut pin cannot block final slot reclaim
- Focused V7 rows green locally in Debug, Release, ASan+UBSan, and TSan

## Still open

- Durable-group Writer publish sites under the flag (partial; sync/async incremental dual-pathed)
- V7 long/multi-OS campaigns and V8 production-flag durable refresh integration soaks
- V11 / V12 worker-affine performance
- Multi-OS CI matrix / soak under the flag
- ADR acceptance

## Commands (local)

```bash
cmake --build --preset macos-debug --target glifistore_tests
build/macos-debug/glifistore_tests 'ADR 0036 production slot V7'
cmake --build --preset macos-release --target glifistore_tests
build/macos-release/glifistore_tests 'ADR 0036 production slot V7'
cmake --build --preset macos-asan --target glifistore_tests
build/macos-asan/glifistore_tests 'ADR 0036 production slot V7'
cmake --build --preset macos-tsan --target glifistore_tests
build/macos-tsan/glifistore_tests 'ADR 0036 production slot V7'
```
