# Local store-put after ADR 0037 combining (debug build)

Architectural smoke only — `macos-debug` (unoptimized). Not comparable to
`local-macos-2026-08-26-head-94f1307` release numbers for gate claims.

| Filter | workers/threads | median ops/s (n=9) |
| --- | --- | --- |
| store-put | 1/1 | ~22407 |
| store-parallel-put | 4/4 | ~50153 |

Litmus: embedded volatile omits dedicated Writer; durable_sync combines under token;
daemon GET visibility barrier + mutation windows ≤32. RAW / fail-closed unchanged.
