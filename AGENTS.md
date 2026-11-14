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
