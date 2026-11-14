# Engineering baseline (Phase A)

**Historical** · assurance program Phase A baseline, 0.1.x · 2026-08-29

The gap reconnaissance that preceded the machine-readable assurance system under `engineering/`. It
is the Phase A input snapshot, not the current gate view, and it advances no release level:
GlifiStore remains an architectural prototype.

The gap rows are not a current backlog; several items they call absent are now implemented. The
current state is authoritative only in
[`engineering/gates/quality-gates.yaml`](../../engineering/gates/quality-gates.yaml) and its
generated [production readiness view](../production-readiness.md).

## State at the baseline

| Area | Present | Notes |
| --- | --- | --- |
| Narrative readiness checklist | `docs/production-readiness.md` | Checkbox model; now derived from `engineering/gates/` |
| Normative specifications | `docs/spec/*` | Architecture, persistence v1, wire v2, client semantics, concurrency, recovery matrix, index, benchmarks |
| ADRs | `docs/adr/` | Including paired shards (0031), paired Store concurrency (0032), TLS/authz, keyed routing |
| CI | `.github/workflows/*` | Build/test, sanitizers and fuzz smoke, durability E2/E3 rehearsal, release-compat, supply-chain, soak, BSD |
| Crash and recovery proofs | `tests/crash/`, integration recovery | E2 process-kill; not physical E3/E4 |
| SDKs | C++, Python, Perl, Go, Erlang, Ruby | Shared fixtures and the client-semantics contract |
| Daemon runtime | Paired Reader–Writer only | The volatile engine under `src/experimental/` is lab-only |

## Gaps (doc ↔ code ↔ evidence)

| Gap | Severity | Priority | Dependencies | Residual risk if deferred |
| --- | --- | --- | --- | --- |
| No machine-readable requirement IDs (`GS-*`) | High | P0 (Phase A) | Schemas + validator | Silent drift; checklist theatre |
| No formal hazard register | High | P0 (Phase A) | Requirements linkage | Untracked integrity failures |
| Gates not multi-state, not CI-enforced | High | P0 (Phase A) | Evidence paths | Closed boxes without proofs |
| Monolithic oversized sources (Phase C + WAV-001/002/003 splits) | Medium | P2 (Phase C) | Dependency matrix | Done; size waivers revoked (audit trail only) |
| No formal linearizability checker | High | P1 (Phase B) | History recorder | Concurrency regressions undetected |
| Actions not pinned to commit SHA (Phase D: SHA pin + validator) | Medium | P2 (Phase D) | Supply-chain policy | Done baseline; Dependabot must update SHAs |
| Physical E3/E4 durability unset | High (claim honesty) | Keep open | Hardware campaign | False durability confirmation |
| Unified error taxonomy across wire, embedded and SDK | Closed (Phase B) | — | `docs/spec/error-taxonomy-v1.md` + `tests/fixtures/error_taxonomy_v1.json` | Transport and deadline vectors still narrative-only beyond the wire fixture |
| `legacy_mutex` policy formalized | Closed (Phase B) | — | `docs/architecture/legacy-mutex-policy.md` | Physical mutex removal still pending product 0.2 |
| Performance budgets not gate-linked (Phase E: budget catalog) | Medium | P2 (Phase E) | Hardware CI | Done baseline; absolute claims wait for `glifistore-linux-perf` |

## Strategy

1. Phase A (this delivery): schemas, requirement catalog bootstrap, hazard register (§7),
   multi-state gates, validator CI, generated readiness view. The claim ceiling stays
   architectural prototype.
2. Phase B: concurrency linearizability, fault hooks, memory-order inventory and reduced TLA+;
   fault matrix honesty; unified error taxonomy; `legacy_mutex` freeze and removal policy.
3. Phase C: CMake component split, dependency matrix, complexity and waiver CI.
4. Phase D: N↔N-1 compatibility matrix, SHA-pinned Actions, claims on tags.
5. Phase E: performance budgets with hardware-gated release thresholds; soak and overload linked to
   gate IDs.

## Authority rule

- Structured data under `engineering/` is the single authority for requirements, hazards and gates.
- Markdown under `docs/assurance/` and `docs/production-readiness.md` is generated or descriptive.
- Specifications and ADRs remain normative for on-disk and wire behavior; requirements reference
  them and do not replace them.

## Phase A acceptance

- `engineering/tools/validate_assurance.py` exits 0 locally and in CI
  (`.github/workflows/assurance.yml`).
- At least one requirement file per category directory listed in the program plan.
- The hazard register covers every event listed in §7 of the brief.
- The production-readiness view is generated from gates, or clearly non-authoritative.
- README, CONTRIBUTING and `AGENTS.md` point at the assurance system.
- No production-ready language beyond the existing prototype disclaimers.
