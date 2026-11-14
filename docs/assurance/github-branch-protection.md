# GitHub branch protection and security settings

**Normative** · GitHub branch protection, security products, required checks · reviewed 2026-08-26

The repository settings that make CI evidence enforceable on `main`. Workflow YAML cannot enable
organization or repository settings, so maintainers apply this checklist in the repository
settings. Green required checks do not imply E3/E4 certification, absolute performance budgets or
production readiness; the claim ceiling stays architectural prototype.

## Required status checks

Require these workflows and jobs to pass before merge. The set is a suggestion; adjust the names to
match the Actions UI.

| Workflow | Why |
| --- | --- |
| `CI` (`strict-iso-hardening`, `build-test`, `sdk-clients`, `install-consumer`, `release-optimized`) | Core correctness and install |
| `Assurance` | Catalog and Actions SHA pins |
| `Sanitizers` | ASan/UBSan and TSan (fuzz on schedule) |
| `Static analysis` | clang-tidy and format |
| `Supply chain scan` | gitleaks and Trivy |
| `Dependency review` | Pull request dependency gate |
| `Actionlint` | Workflow YAML and pin validator (when workflows change) |
| `CodeQL` (`analyze-cpp` at minimum) | SAST |
| `OpenSSF Scorecard` | Supply-chain posture |
| `FreeBSD` / `OpenBSD LibreSSL` | Supported Unix portability |
| `Released artifact compatibility` | Fixture and self-artifact harness |

Require the correctness, portability and distribution jobs above on `main`. Do **not** require
absolute performance budgets, hard-pinned scaling or physical E3/E4 jobs as merge blockers:
performance evidence stays labeled `hardware` and is independent of the release sealing residuals
([evidence taxonomy](evidence-taxonomy.md),
[release residuals](../distribution/wave5-l7-residuals.md)).

Optional, because they are path-filtered or scheduled; require one only if it always runs on the
pull request:

- `Docs links`, `License check`, `Formal models (TLC)` (ShardPair + PersistenceRecovery matrix)
- `release-lto-smoke` (heavier; keep required once stable)

Do not require `Coverage` (diagnostic), `Extended soak`, `Benchmarks` absolute thresholds,
`Paired Linux hard-pinned A/B` (self-hosted) or physical E3 jobs.

## Security products

Enable in **Settings → Security**:

1. Secret scanning and push protection (public repositories: enable both).
2. Private vulnerability reporting, which aligns with `SECURITY.md`.
3. Code scanning, so Scorecard, CodeQL and Trivy SARIF can publish when the repository variable
   `GLIFISTORE_UPLOAD_SARIF=true` is set.
4. Dependabot alerts and security updates, for the ecosystems configured in
   [`.github/dependabot.yml`](../../.github/dependabot.yml).

## Merge queue

After the required checks have been stable for several weeks, enable a merge queue on `main` with
the same required checks. Prefer squash merges for a linear history.

## Actions SHA discipline

Third-party Actions stay on 40-character commit SHAs
([Actions pinning](../security/actions-pinning.md)). A Dependabot pull request that only bumps a
floating tag must be rewritten to a SHA before merge.

## Residuals

- A queued, skipped, cancelled or unavailable hosted run is not green evidence; a local green run is
  not CI green.
- Physical E3/E4, absolute hardware performance and tagged N−1 fixture drops remain outside this
  checklist.
- Producers and validators may be structurally green while retained tagged evidence (ABI N−1, BSD
  packages, Cosign/SBOM pointers) is still absent; see the
  [release residuals](../distribution/wave5-l7-residuals.md).
