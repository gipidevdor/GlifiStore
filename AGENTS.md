# Agent guidance for GlifiStore

GlifiStore is an **architectural prototype**. Do not claim production readiness,
alpha/beta/stable closure, E3/E4 durability certification, or “ready for hostile
public deployment” unless the corresponding quality gates in `engineering/gates/`
are in `PROVATA_IN_CI` / `ACCETTATA_PER_RILASCIO` with existing evidence paths.

## Guiding principle

A promise is credible only when it is the conjunction of:

1. a machine-readable **requirement** (`engineering/requirements/`);
2. a normative **specification** or accepted ADR (`docs/spec/`, `docs/adr/`);
3. an **implementation** that matches the contract;
4. automated **proofs** (tests, fuzz, crash, matrices);
5. retained **evidence** (CI artifacts, campaign records under `engineering/evidence/`);
6. an explicit **residual risk** (including hazards in `engineering/hazards/`).

Design text or code presence alone never closes a gate.

## Non-negotiables

- C++23; CMake ≥ 3.25.
- Persistence format **v1** and wire protocol **v2** unless an ADR explicitly versions a change.
- Official runtime: paired Reader–Writer shard pairs (ADR 0031/0032); no silent dual-runtime.
- Partition ownership, immutable read generations, bounded lanes, fail-closed recovery.
- No unbounded client-controlled work; no raw owning `new`/`delete` in production paths.
- No persisted struct casts; explicit little-endian codecs (`include/glifistore/core/little_endian.hpp`);
  checked size/offset/capacity arithmetic.
- macOS, Linux, FreeBSD, and OpenBSD remain in documented scope; durability claims are
  platform-row specific (see platform durability evidence matrix).

## Silent-change ban

Do not silently change acknowledgement points, persistent write ordering, Manifest authority,
routing, tombstone/erase semantics, mutation visibility, borrow lifetimes, recovery behavior,
on-disk/wire compatibility, or official client observable outcomes. Such changes require a new
or updated ADR plus linked requirements, proofs, and evidence.

## Assurance system

### GDev and coding agents

The versioned GDev project is `.gdev-assurance/project.eld`; everyday operations
in `.gdev-project.eld` wrap the existing developer script. Follow
`docs/development/gdev.md` for CLI/MCP, procedures and agent sessions.
The YAML catalogs under `engineering/` remain authoritative. After editing them,
validate assurance, regenerate the GDev view with `export_gdev_catalog.py`
and run `engineering/tools/gdev_verify.py metadata` with the local Python.
Do not promote a source gate because a GDev procedure or derived catalog passes.
For each reproducible integration error, fix the responsible layer, add a reusable
regression and repeat the affected cross-project flow; retain failed attempts and
stop repeated retries that produce no new evidence.

Before selecting a correction, read the relevant current hazards and their linked
requirements, authority and residual risks. For shard capacity and client routing,
start with `engineering/hazards/routing-capacity.yaml` (HAZ-045/046), ADR 0033
and `docs/spec/client-semantics-v1.md`. Evaluate skewed load separately from
compaction progress; aggregate throughput does not demonstrate hot-shard isolation.
Treat intended rejections such as WRONG_OWNER according to the client contract.
Changes to forwarding, rerouting, retry or ownership semantics require the ADR and proofs
specified by the silent-change ban. Include HAZ-044's remaining pending-group
failure paths when changing admission or compaction conflicts. A successful local
check does not accept an open hazard or eliminate its residual risk.

| Path | Role |
| --- | --- |
| [`docs/assurance/engineering-baseline.md`](docs/assurance/engineering-baseline.md) | Gap recon and Phase A baseline |
| [`engineering/`](engineering/) | Authority for requirements, hazards, gates, waivers, claims |
| [`engineering/tools/validate_assurance.py`](engineering/tools/validate_assurance.py) | Schema + referential integrity + CI gate |
| [`docs/production-readiness.md`](docs/production-readiness.md) | **Generated** view of gates (do not edit by hand) |

Run locally:

```bash
python3 -m pip install --user PyYAML jsonschema
python3 engineering/tools/validate_assurance.py
python3 engineering/tools/validate_assurance.py --write-generated
```

## Change discipline

Use Conventional Commits. Prefer one purpose per commit. Update requirements/gates/hazards when
behavior or evidence changes. Keep README status as architectural prototype until gates justify
otherwise.

## C++ code style — every change

Follow [`docs/development/code-style.md`](docs/development/code-style.md) for all new or modified
native code. `.clang-format` is the layout authority; `.clang-format-version` pins the exact tool.
Run `./scripts/dev.sh format` and `./scripts/dev.sh format-check` before handoff. The same checker
runs in CI and covers tracked and untracked native sources, including C ABI consumers, tools and
probes. Keep descriptive names with units, explicit ownership/error contracts, braced control flow
and direct includes. Comments explain invariants and reasons. Do not add blanket lint suppressions,
enable every analyzer check indiscriminately, or use cosmetic work to change runtime semantics.
Keep mechanical formatting separate from behavior changes and retain appropriate verification.

## Disk hygiene — every run

Builds, sanitizer trees, benchmark data and test stores are large; several agents working at once
have filled the disk before. Leave the machine as you found it:

- Reuse one build tree per preset; do not create extra copies in temporary directories.
- Write scratch output (logs, benchmark results, probe data) to your scratch directory, never to the
  repository or `/tmp`, and delete it before handoff.
- Before handoff, delete build trees you created for this task only (for example a sanitizer or
  release preset you configured), benchmark data directories and temporary Stores. Remove a git
  worktree you created with `git worktree remove` once its branch is merged or abandoned.
- Benchmarks that write durable data must use a bounded size and a directory you remove afterwards.
- Check free space (`df -h`) before long sanitizer, crash or benchmark campaigns; stop and report
  instead of filling the disk.
