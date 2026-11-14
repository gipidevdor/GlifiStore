# Platform durability evidence — `freebsd-ufs`

**Status:** placeholder only. **Not E3 certified. Not E4 certified.**

This directory is a reserved evidence path for future pinned-row campaign artifacts
for the `freebsd-ufs` platform/filesystem row. Passing CI, E2 process-kill, or E3
block-reset *rehearsal* harnesses does **not** populate or certify this row.

## Evidence path slots (empty until a reviewed campaign lands)

| Slot | Purpose | Status |
| --- | --- | --- |
| `e0-metadata/` | Codec/unit reopen provenance | empty |
| `e1-fault-injection/` | Deterministic FS fault matrix artifacts | empty |
| `e2-process-kill/` | Retained SIGKILL collector runs for this row | empty |
| `e3-campaign/` | Abrupt reset / power-cut campaign (pinned hardware or named guest/host boundary) | empty |
| `e4-release/` | Repeated release campaign with checksummed provenance | empty |

See [platform durability evidence matrix](../../../../docs/architecture/platform-durability-evidence.md)
and `GS-PERSIST-FAULT-001`. Do not invent certification claims.
