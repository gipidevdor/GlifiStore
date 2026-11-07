Status: normative for CI workflow composition
Applies to: `.github/workflows/*.yml` third-party Actions
Owner: maintainers
Last reviewed: 2026-11-07

# GitHub Actions commit pinning

Third-party Actions must be pinned to a **40-character commit SHA**. Optional trailing comments
may record the human tag (for example `# v7`). Mutable refs (`@v1`, `@main`, short SHAs) are
forbidden.

Authority:

- Validator: `python3 engineering/tools/validate_actions_pins.py`
- Requirement: `GS-SUPPLY-ACTIONS-001`
- Gate: `GATE-THREAT-SUPPLY`

When updating an Action, change the SHA and the comment together. Dependabot PRs that only bump
a floating tag must be rewritten to a SHA before merge. `validate_actions_pins.py` also checks that
[`.github/dependabot.yml`](../../.github/dependabot.yml) declares `package-ecosystem: github-actions`
and carries an explicit SHA / 40-character / floating-tag discipline comment. Dependabot also
watches `pip` (`/sdk/python`) and `gomod` (`/sdk/go`); GitHub Actions updates remain SHA-pinned.

All `github/codeql-action/*` steps must use the same commit SHA across workflows: updating
`init` alone leaves `analyze` incompatible with its generated configuration. The validator rejects
mixed revisions. Dependabot's `codeql` and `codeql-security` groups update `init`, `analyze`, and
`upload-sarif` together for version and security updates respectively, using
[dependency groups](https://docs.github.com/en/code-security/reference/supply-chain-security/dependabot-options-reference#groups).

Least-privilege and CI overlap notes (Wave 5 honesty):

- Candidate / verify default to `contents: read`; only publish elevates `contents: write` behind the
  protected `release` Environment; signing jobs elevate `id-token` / `attestations` only when needed.
- Overlapping PR vs tag scans (Trivy/gitleaks in `supply-chain-scan.yml` and tag
  `security-supply-chain`) are intentional: PR regression gate ≠ tag-retained matrix evidence.
  Do not prune the tag path while `security-matrix-evidence` requires those logs.
- See [`wave5-l7-residuals.md`](../distribution/wave5-l7-residuals.md).

Additional supply-chain CI: OpenSSF Scorecard (`.github/workflows/scorecard.yml`),
dependency-review on PRs, and actionlint + `validate_actions_pins.py` on workflow changes.
Repository settings checklist: [github-branch-protection](../assurance/github-branch-protection.md).
