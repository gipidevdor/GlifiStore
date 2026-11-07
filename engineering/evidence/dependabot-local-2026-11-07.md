# Dependabot workflow repair — local campaign, 2026-11-07

Scope: Dependabot-related working-tree changes based on `62d87cda086f06b4a11bfaf197cad13bae9ad2ae`.
This is local prototype evidence. The updated workflows still require hosted CI;
GATE-THREAT-SUPPLY remains IMPLEMENTATA and GS-SUPPLY-ACTIONS-001 remains implementato.

## Observed failures

- [Dependabot PR 61, CodeQL run](https://github.com/gipidevdor/GlifiStore/actions/runs/42332163971)
  failed because `init` produced version 4.39.3 configuration while `analyze` used 4.39.1.
  All 16 CodeQL references now use the v4.39.3 commit. The pin validator rejects mixed
  revisions, and version/security Dependabot groups cover all CodeQL sub-actions.
- [Dependabot PR 63, TLC run](https://github.com/gipidevdor/GlifiStore/actions/runs/42332173063)
  installed Java successfully, then received HTTP 404 for TLA+ asset 692577622.
  The replacement [v1.8.0 asset](https://api.github.com/repos/tlaplus/tlaplus/releases/assets/718777374)
  was downloaded and its bytes independently hashed to
  `e1f11b75c4b5bc4478ba0ac5cb6ae28e40a01b0016bc33e0aa97da22dfdbcd7f`, matching
  the GitHub asset metadata. The workflow pins both asset ID and digest.

## Dependency pins

Each tag was resolved using `git ls-remote`, including annotated-tag dereferencing.

| Action | Version | Commit |
| --- | --- | --- |
| github/codeql-action | v4.39.3 | 33d7b85da1a04171e211befe2c739303b9a4c066 |
| actions/setup-java | v6.2.0 | 77dd5b2ce225ea11469928ac317850245a74ae78 |
| vmactions/openbsd-vm | v1.4.9 | f1ca8bc8ecc5b6ef7218e5b914836e067e1546d3 |
| ossf/scorecard-action | v2.5.0 | fa7117db83c52d787ae5a89f350999701505d78e |
| sigstore/cosign-installer | v4.1.3 | 756fe2d284c7292c4e9b62c8a28f1e5629c59532 |

OpenBSD stays explicitly on release 7.9 in both affected workflows.

## Local proofs

The six new [pin/grouping regressions](../../scripts/tests/test_actions_pins.py) cover
matching CodeQL revisions, a partial `init` upgrade, stale release-workflow pins,
unrelated Action revisions, mutable refs, and grouping for both update types.

| Check | Result |
| --- | --- |
| `python -m unittest discover -s scripts/tests` | 438 tests passed on Python 3.14.10; [retained log](dependabot-2026-11-07/python314-tests.log) |
| `actionlint -color` | Passed with actionlint 1.7.12 |
| `python engineering/tools/validate_actions_pins.py` | 217 SHA-pinned uses across 26 workflows; consistent CodeQL revisions |
| `python engineering/tools/validate_assurance.py --write-generated`, then validation | 36 requirements, 34 hazards, 30 gates, 3 waivers; zero warnings; generated views unchanged |
| `validate_documentation.py`, including this new record | No unresolved repository-local links |
| `python engineering/tools/validate_claims.py` | Passed, one claim |
| `git diff --check` | Passed |

Python 3.14 matches the CI interpreter. The initial system-Python 3.9.6 full-suite run
failed two existing benchmark parser tests because `zip(strict=True)` requires Python 3.10+;
the final Python 3.14 run above passes both without changes to benchmark code.

Both model runners used the downloaded jar on macOS arm64 / Amazon Corretto 11.0.26:

| Model | Result | Retained log |
| --- | --- | --- |
| ShardPair | 748 generated / 204 distinct states; invariants and liveness passed | [TLC log](dependabot-2026-11-07/shard-pair-tlc.log) |
| PersistenceRecovery | 37 generated / 37 distinct states; invariants and both temporal properties passed | [TLC log](dependabot-2026-11-07/persistence-tlc.log) |

Commands: set `TLA2TOOLS_JAR` to the downloaded and hash-verified jar, then run
`bash engineering/formal/shard_pair/run-tlc.sh` and
`bash engineering/formal/persistence/run-tlc.sh`, each with a 480-second process timeout.

## Residual limits

The upstream v1.8.0 tag is republished. Deleting its pinned asset again requires a new
download/hash review; a download error or digest mismatch remains blocking.
These local checks do not execute hosted CodeQL, OpenBSD VMs, Scorecard publication,
or keyless release signing. The hosted TLC workflow uses Java 21. No runtime,
wire/persistence contract, durability status, or release claim changes here.
