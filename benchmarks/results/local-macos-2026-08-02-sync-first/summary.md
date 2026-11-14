# Lab: Writer sync scheduling experiments (2026-08-02)

Claim ceiling: Apple M4 / `macos-release` lab evidence only.

## Phase attribution (phases ON; absolute ns include timer overhead)

On `store-put` (instrumented binary), relative Writer vs caller:

| Phase | mean_ns | note |
| --- | ---: | --- |
| `worker_apply` | ~381 | Store append + Index publish |
| `publish` | ~640 | `publish_incremental` |
| `ack` | ~3080 | Caller wait (includes apply+publish+wake) |

Implication: ~2 µs of ack is scheduling/wake beyond apply+publish. Cutting publish-only cannot reach 600 k alone.

## Rejected scheduling tweaks

| Change | `store_put` 1t | Notes |
| --- | ---: | --- |
| Post-sync Writer spin 64 + sync-first | ~170 k | Hard reject |
| Sync-first only (no spin) | ~348–363 k | No win vs merge-first ~377 k |
| Merge-first (status quo) | ~377 k | Kept |

Evidence raw files under this directory and
`local-macos-2026-08-02-embed-delta/attribution-phases.txt`.

## Kept from this session

- Embed `DeltaState` + `make_shared` generation (see embed-delta / make-shared-gen labs)
- `GS_PHASE_PUT(worker_apply)` / `publish` scopes on sync volatile path (lab-only when phases ON)
