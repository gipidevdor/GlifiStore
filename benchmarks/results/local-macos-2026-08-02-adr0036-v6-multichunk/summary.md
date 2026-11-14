# Paired volatile multi-chunk sticky fail-closed

Date: 2026-08-02
Preset: `macos-release` (Apple Silicon)
Harness: `glifistore_allocation_fault_tests` (process-wide allocation fault arm)

## Hole fixed

Volatile sync Writer drained `put_batch` as ≤32 publish chunks. After
`publish_fail_closed()` in an early chunk, later chunks could still
`put_volatile_published` + publish + ACK (durable was masked by catalog health;
async by `expire_remaining_`).

## Fix

Reject remaining sync nodes when `!healthy_` at the start of each volatile chunk
and after each chunk completes; same guard on durable single-node sync drain.

## Result

Full allocation-fault suite including `run_paired_volatile_multichunk_fail_closed`
passed (`allocation fault injection passed`).
