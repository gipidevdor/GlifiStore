# Contributing

GlifiStore is an architectural prototype under active design. A promise in it holds only as the
conjunction of a requirement, a specification, an implementation, automated proofs, retained
evidence and a stated residual risk. [AGENTS.md](AGENTS.md) states that rule and the
non-negotiables; read it first.

## Loop

1. On macOS, bootstrap the local toolchain with `./scripts/bootstrap-macos.sh`.
2. Work on a focused branch.
3. Run `./scripts/dev.sh format`, `./scripts/dev.sh verify`, `./scripts/dev.sh test` and the
   relevant sanitizer. `verify` changes nothing: it checks formatting, shell syntax, documentation,
   assurance, packaging policy and Python tooling.
4. Add a regression test for every correctness or memory-safety fix.
5. Update the ADR when a fixed architectural decision changes.
6. When a promised behavior or a gate changes, update `engineering/` and run
   `python3 engineering/tools/validate_assurance.py` (with `--write-generated` when gates change).

## Rules

- Persistence work must follow the
  [durability and recovery contract](docs/architecture/durability-recovery.md). Public API work must
  preserve the ownership and compatibility rules of the
  [public C++ API](docs/reference/cpp-api.md).
- Changing a commit point, routing metadata, recovery authority or read lifetime requires a new ADR.
- Commits follow Conventional Commits.
- Public terminology follows the architecture documents and the [glossary](docs/glossary.md): the
  public model has a Store, Workers, an Index and Segments.
- No text-protocol compatibility layer and no unbounded client-controlled work without an accepted
  design change.
- No claim of production readiness, and no gate moved to `ACCETTATA_PER_RILASCIO` or
  `PROVATA_IN_CI`, without linked proofs and existing evidence paths.
- Documentation follows the [writing standard](docs/development/documentation.md).

## Safety gates

- No raw owning pointers or manual `new`/`delete` in production code.
- No persisted struct casts; use the explicit little-endian codecs in
  `include/glifistore/core/little_endian.hpp`.
- No unchecked length, offset, capacity or allocation arithmetic.
- No Record parser without malformed-input tests and fuzz coverage.
- No reclaim while a reader may still address a Segment.
- No performance claim without a reproducible benchmark and a hardware description.

## Assurance

The authority is [`engineering/`](engineering/README.md): requirements, hazards and gates. The
[quality gates](docs/production-readiness.md) are a view generated from it, the
[engineering baseline](docs/assurance/engineering-baseline.md) records where the program started,
and `.github/workflows/assurance.yml` enforces it in CI.

## Licensing

GlifiStore is BSD-3-Clause ([LICENSE](LICENSE)), copyright (c) 2026 Giacomo Picchiarelli.
Third-party attributions are in [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); contributor and redistributor rules in the
[licensing policy](docs/legal/licensing.md).

By contributing, you license your contribution under BSD-3-Clause and confirm that you have the
right to do so. Do not add exclusive GPL, AGPL or SSPL runtime dependencies. New substantial source
files should carry SPDX headers:

```text
// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: 2026 Giacomo Picchiarelli
```
