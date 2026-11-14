# ADR 0036 gates V6 / V8 / V14 — lab evidence (status-quo + prototype)

Date: 2026-08-02
Preset: `macos-release` (Apple Silicon); V6 also checked under `macos-asan`
Claim ceiling: lab evidence only; does not accept ADR 0036 for production slot-pool landing.

## Commands

```text
./build/macos-release/glifistore_tests 'ADR 0036'
./build/macos-asan/glifistore_tests 'ADR 0036 V6'
./build/macos-release/glifistore_tests 'paired Reader refreshes'
./build/macos-release/glifistore_tests 'durable read catalog refresh'
./build/macos-release/glifistore_tests 'paired Store put_batch'
./build/macos-release/glifistore_allocation_fault_tests
./build/macos-release/glifistore_crash_daemon --daemon ./build/macos-release/glifistored \
  --storage durable-sync --checkpoint pre-commit
./build/macos-release/glifistore_crash_daemon --daemon ./build/macos-release/glifistored \
  --storage durable-sync
./build/macos-release/glifistore_crash_persistence --mode group-matrix
```

## Results

| Gate | Evidence | Result |
| --- | --- | --- |
| V6 prototype | `ADR 0036 V6…` (Release + ASan): rejected keys never GET-visible; fail-closed completion before release-store | 0 failures |
| V6 production-baseline | `glifistore_allocation_fault_tests` (post-write / publication allocation fail-closed) | passed |
| V8 production-baseline | durable catalog refresh / rotation / isolation filters | 3/3 pass |
| V10 production-baseline | `paired Store put_batch` RAW + same-key FIFO | 2/2 pass |
| V14 production-baseline | crash-daemon pre-commit + post-ack sync; crash_persistence group-matrix | ok |

Full sync crash matrix and multi-OS CI remain open for a future production slot-pool candidate.
