# Assurance catalog

The machine-readable authority for GlifiStore's requirements, hazards, quality gates, waivers,
claims and evidence pointers. [Quality gates](../docs/production-readiness.md),
[gate list](../docs/assurance/gates.md) and [hazard register](../docs/assurance/hazards.md) are
generated from this tree.

GlifiStore remains an architectural prototype. A closed checklist box is not production readiness:
a claim level rises only when the residuals that match it close with retained evidence under
`claims/` and `evidence/`.

## Layout

| Path | Contents |
| --- | --- |
| `schemas/` | JSON Schemas for requirements, hazards, gates and waivers |
| `requirements/` | `GS-*` requirements by category |
| `hazards/` | The hazard register |
| `gates/` | Multi-state quality gates |
| `waivers/` | Time-bounded waivers; CI rejects expired ones |
| `build/` | CMake dependency matrix and structure-debt thresholds |
| `compatibility/` | The N↔N-1 matrix |
| `distribution/` | The package backend matrix: backends, targets, CI profiles, check vocabulary |
| `performance/` | Hosted and hardware budgets, linked to soak and overload |
| `claims/`, `evidence/` | Release claims and retained evidence pointers |
| `formal/shard_pair/` | Reduced TLA+ shard-pair model and TLC helper |
| `formal/persistence/` | Abstract TLA+ model of write, sync, commit slot, Manifest and recovery |
| `tools/validate_assurance.py` | Catalog validator and generator of the Markdown views |
| `tools/validate_documentation.py` | UTF-8 and repository-local link integrity of all tracked Markdown |
| `tools/run_clang_tidy_gate.py` | Fail-closed high-signal diagnostics over the production compile database |
| `tools/validate_cmake_deps.py` | CMake layout and include rules |
| `tools/validate_structure_debt.py` | Size and TODO thresholds, with waivers |
| `tools/validate_actions_pins.py` | SHA-pinned GitHub Actions |
| `tools/validate_compat_matrix.py` | The N↔N-1 matrix |
| `tools/validate_claims.py` | The release claim schema |
| `tools/validate_perf_budgets.py` | The performance and soak budget catalog |
| `tools/release_identity.py`, `release_bundle.py` | Tag identity, manifests, checksums and transitive seals |
| `tools/prior_release.py` and the fixture validators | Fail-closed selection of the prior ABI and wire release and extraction of its retained consumer |
| `tools/compare_release_rebuild.py` | Closed-set comparison of an independent release archive and its build authority |
| `tools/validate_bsd_packaging.py` | Reference-port invariants and native release prerequisites |
| `tools/generate_package_matrix.py` | Package matrix validation and profile expansion |
| `tools/generate_package_status.py` | Generator of [package status](../docs/distribution/package-status.md) |
| `tools/validate_package_evidence.py` | Fail-closed package evidence: a result never exceeds its checks |
| `tools/upgrade_baseline.py`, `n1_package_artifacts.py`, `validate_package_admission.py`, `run_package_admission.py` | SemVer-aware sealed N-1 selection; install, seed, upgrade and verify for Linux deb and rpm, FreeBSD, OpenBSD, MacPorts and Homebrew when `GLIFISTORE_N1_PACKAGE_DIR` supplies sealed predecessor bytes; exact-byte package admission. Wired in `package-ci.yml` and `release.yml`; a positive admission and a sealed N-1 pass remain open |

## Validate

```bash
python3 -m pip install PyYAML jsonschema
python3 engineering/tools/validate_assurance.py
python3 engineering/tools/validate_assurance.py --write-generated
python3 engineering/tools/validate_documentation.py
python3 engineering/tools/run_clang_tidy_gate.py --build-dir build/unix-clang-tidy
python3 engineering/tools/validate_cmake_deps.py
python3 engineering/tools/validate_structure_debt.py
python3 engineering/tools/validate_actions_pins.py
python3 engineering/tools/validate_compat_matrix.py
python3 engineering/tools/validate_claims.py
python3 engineering/tools/validate_perf_budgets.py
python3 engineering/tools/validate_bsd_packaging.py
python3 engineering/tools/generate_package_matrix.py validate
python3 engineering/tools/generate_package_status.py
python3 engineering/tools/generate_package_status.py --write-generated
python3 -m unittest scripts.tests.test_release_artifacts scripts.tests.test_bsd_packaging \
  scripts.tests.test_artifact_release_workflow
```

CI runs them in `.github/workflows/assurance.yml`, except the clang-tidy gate, which runs in
`static-analysis.yml`, and the BSD packaging check, which runs in `release.yml`. The two
`--write-generated` commands are local: they refresh the generated views, which CI only checks for
staleness. A local pass does not replace hosted evidence: before treating a commit as green, confirm
that every required workflow completed for that exact commit and that the required evidence
artifacts were retained.
