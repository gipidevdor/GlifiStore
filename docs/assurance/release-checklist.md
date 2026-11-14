# Release checklist

**Descriptive** · tagged releases and claim packaging, GlifiStore 0.1.x · reviewed 2026-08-26

The steps for cutting a version tag. Completing them does not by itself raise the claim ceiling
above architectural prototype; gate authority is
[`engineering/gates/quality-gates.yaml`](../../engineering/gates/quality-gates.yaml).

## Before the tag

- [ ] `LICENSE`, `NOTICE` and `THIRD_PARTY_NOTICES.md` are still accurate for any new dependency
- [ ] Local: `./scripts/ci-license-check.sh`
- [ ] `CHANGELOG.md` Unreleased section reviewed and the version section drafted
- [ ] Persistence, wire and API changes have matching fixtures and matrix rows
  (`engineering/compatibility/n-n1-matrix.yaml` if the N↔N-1 policy changed)
- [ ] Local: `python3 engineering/tools/validate_assurance.py`
- [ ] Local: structure, Actions, compatibility, claims and performance-budget validators (see
  `engineering/README.md`)
- [ ] Local: `ctest` / `glifistore_tests` green on at least one supported host
- [ ] Sanitizer and crash suites considered for the change set, or deferred with a residual note
- [ ] No new absolute performance claims without `glifistore-linux-perf` `pass-candidate` evidence

## Tag packaging

- [ ] Create the annotated tag `vX.Y.Z`; `VERSION == X.Y.Z` and a clean exact-tag checkout are
      mandatory
- [ ] FreeBSD/OpenBSD `PORTS_ACCOUNT_REGISTERED` markers exist only after upstream ports allocation;
      native `distinfo` and package evidence come from tagged producers (never hand-written digests)
- [ ] All eight artifact-bound release evidence JSON files and their referenced logs pass
      `engineering/tools/release_evidence.py --require-ci` (see
      [`engineering/evidence/release/`](../../engineering/evidence/release/README.md))
- [ ] The candidate contains the deterministic compiled ABI consumer archive and its bound SPDX SBOM
- [ ] The candidate contains the deterministic compiled wire-v2 client archive and its bound SPDX
      SBOM
- [ ] `scripts/package-release-compatibility-artifacts.sh X.Y.Z`
- [ ] `scripts/package-release-claim.sh X.Y.Z <40-char-sha>`
- [ ] Decide whether to commit `tests/fixtures/released/X.Y.Z/` as a permanent N−1 drop
- [ ] Package and commit `tests/fixtures/released-stores/X.Y.Z/` from the stopped, installed,
      tagged daemon's Store, so the next version has a complete prior persistence baseline
- [ ] The candidate workflow builds once and produces `candidate-seal.json`
- [ ] Verify creates `candidate-admission.json` before executing or importing any post-build input
- [ ] Same-run certification artifacts are imported only through `import-evidence`; the closed
      `evidence-import.json` inventory validates before release-wide policy evaluation
- [ ] Persistence evidence selected a strictly older tagged Store and ran only the candidate daemon
      and tools; the absence of a prior baseline remains a hard failure, not a waiver
- [ ] ABI evidence revalidated and attested the complete prior release, proved loader resolution,
      and ran both the old-consumer/new-library and new-consumer/old-library directions
- [ ] Wire evidence revalidated and attested the complete prior release, then ran new/new,
      old-client/new-server and new-client/old-server using only retained compiled clients
- [ ] Security evidence ran the full ASan+UBSan/TSan suites, four-language CodeQL, strict/static and
      supply-chain checks, then validated the candidate SBOMs and the exact distributed Linux ELF
- [ ] FreeBSD evidence generated `distinfo` from the sealed source, built and installed the native
      package, exercised rc.subr and durable recovery, then proved config/data preservation
- [ ] OpenBSD evidence generated `distinfo` from the sealed source, built and installed the native
      package, exercised `rcctl` and durable recovery under `/var/glifistore`, then proved
      config/data preservation
- [ ] Optional package backends (`deb`, `rpm`, `macports`, `homebrew`) ran the `release` profile
      from the sealed candidate; their evidence stays diagnostic and admits no release artifact
      ([package CI](../distribution/package-ci.md))
- [ ] Any package admission was produced by `run_package_admission.py` (or the
      baseline/manifest/admission tools it orchestrates) against retained CI evidence and retained
      as an artifact; a locally produced report is not admissible
- [ ] Reproducibility evidence rebuilt the closed four-archive set on a distinct runner, matched
      every size and SHA-256, and kept rebuilt bytes outside candidate import and promotion
- [ ] Verify consumes those exact bytes, tests the installed archive, emits the manifest and
      checksums, and attests `verified-seal.json`
- [ ] The protected `release` Environment approves promotion only after the technical gates pass
- [ ] Publish re-verifies the offline Sigstore bundle and uploads without rebuild or clobber (an
      existing release tag is refused; never `--clobber`)
- [ ] Upload and retain CI artifacts under the `engineering/evidence/` pointers in the claim YAML
      (release checklist scaffolding: `engineering/evidence/release/`)

## Honesty checks

Do not skip these.

- [ ] Claim YAML `claim_massimo` stays honest (`architectural-prototype` until the alpha criteria
      are met)
- [ ] Durability campaigns still show `e3_certified=no` unless a promoted campaign record exists
- [ ] Every required hosted check completed successfully for the exact commit; queued, skipped or
      cancelled is not green
- [ ] Documentation snapshot: version lifecycle, compatibility manual and secure-profile residuals
      still accurate

## After the tag

- [ ] Retain `candidate-admission.json` and `evidence-import.json` with the verified bundle
- [ ] Verify the `release-compat.yml` and `supply-chain.yml` artifacts for the tag
- [ ] Update the support notes if a prior release becomes N−1 for reopen matrices
- [ ] File the residuals that remain open (E3/E4, hardware performance, hot zero-fence backup, …)

## Related

- [Compatibility and migration](../operations/compatibility-and-migration.md)
- [Package CI operator guide](../distribution/package-ci.md)
- [Package backend status](../distribution/package-status.md) (generated)
- [GitHub branch protection](github-branch-protection.md)
- [Production readiness](../production-readiness.md) (generated)
