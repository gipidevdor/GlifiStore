# GitHub Actions commit pinning

**Normative** · third-party Actions in `.github/workflows/*.yml` · reviewed 2026-11-07

Requirement `GS-SUPPLY-ACTIONS-001`, gate `GATE-THREAT-SUPPLY`. Validator:
`python3 engineering/tools/validate_actions_pins.py`.

How CI workflows must reference third-party GitHub Actions; the rule is normative for CI workflow
composition.

Third-party Actions must be pinned to a 40-character commit SHA. An optional trailing comment may
record the human tag, for example `# v7`. Mutable refs (`@v1`, `@main`, short SHAs) are forbidden.

## Updates

When updating an Action, change the SHA and the comment together. A Dependabot pull request that
only bumps a floating tag must be rewritten to a SHA before merge.

`validate_actions_pins.py` also checks that [`.github/dependabot.yml`](../../.github/dependabot.yml)
declares `package-ecosystem: github-actions` and carries an explicit SHA / 40-character /
floating-tag discipline comment. Dependabot also watches `pip` (`/sdk/python`) and `gomod`
(`/sdk/go`); GitHub Actions updates remain SHA-pinned.

## CodeQL

Every `github/codeql-action/*` step must use the same commit SHA across workflows: updating `init`
alone leaves `analyze` incompatible with its generated configuration, and the validator rejects
mixed revisions. Dependabot's `codeql` and `codeql-security` groups update `init`, `analyze` and
`upload-sarif` together, for version and security updates respectively, using
[dependency groups](https://docs.github.com/en/code-security/reference/supply-chain-security/dependabot-options-reference#groups).

## Least privilege and overlapping scans

- Candidate and verify jobs default to `contents: read`. Only publish elevates to
  `contents: write`, behind the protected `release` Environment; signing jobs elevate `id-token`
  and `attestations` only when needed.
- The overlapping PR and tag scans (Trivy and gitleaks in `supply-chain-scan.yml` and the tag
  `security-supply-chain`) are intentional: the PR regression gate is not the tag-retained matrix
  evidence. Do not prune the tag path while `security-matrix-evidence` requires those logs.
- See [release, portability and supply-chain residuals](../distribution/wave5-l7-residuals.md).

## Related CI

Other supply-chain CI: OpenSSF Scorecard (`.github/workflows/scorecard.yml`), dependency review on
pull requests, and actionlint with `validate_actions_pins.py` on workflow changes. The repository
settings checklist is [GitHub branch protection](../assurance/github-branch-protection.md).
