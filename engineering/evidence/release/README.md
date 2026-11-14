# Release evidence retention (Wave 5 / L7)

Status: checklist scaffolding — **no tagged retention yet**
Authority: [`docs/distribution/release-evidence.md`](../../../docs/distribution/release-evidence.md)
Claim ceiling: architectural prototype

This directory holds **pointers and checklists** for tagged release promotion. It must never
contain hand-written digests, fabricated Cosign bundles, or substitute package bytes.

## Required eight evidence records

After a successful annotated `vX.Y.Z` tag run, retain CI artifacts (or durable pointers to them)
covering every row below. Filenames are fixed by `engineering/tools/release_evidence.py`.

| Record | Producer | Blocked until |
| --- | --- | --- |
| `abi-compatibility-evidence.json` | `release.yml` `abi-compatibility-evidence` | First complete attested prior ABI-1 release + sealed old consumer |
| `persistent-compatibility-evidence.json` | `persistent-compatibility-evidence` | First `tests/fixtures/released-stores/<older-semver>/` drop |
| `wire-compatibility-evidence.json` | `wire-compatibility-evidence` | First complete attested prior wire-v2 client/server assets |
| `sdk-installed-interop-evidence.json` | `sdk-installed-evidence` | Successful tagged same-run retention |
| `security-matrix-evidence.json` | `security-matrix-evidence` | Successful tagged same-run retention |
| `freebsd-package-evidence.json` | `freebsd-package-evidence` | `packaging/freebsd/PORTS_ACCOUNT_REGISTERED` + tagged native run |
| `openbsd-package-evidence.json` | `openbsd-package-evidence` | `packaging/openbsd/PORTS_ACCOUNT_REGISTERED` + tagged native run |
| `reproducibility-evidence.json` | `reproducibility-evidence` | Successful tagged independent rebuild compare |

## Signing / SLSA / SBOM honesty

| Surface | Implemented path | Retained-evidence gap |
| --- | --- | --- |
| SPDX SBOM | Candidate bind-sbom + Syft; FreeBSD/OpenBSD package bind | No tagged SBOM retained under this tree |
| Cosign keyless | `supply-chain.yml` on tags | No offline bundle pointer committed here |
| GitHub SLSA-style attestations | `actions/attest` (public or `ENABLE_ARTIFACT_ATTESTATIONS`) | No successful tag provenance retained here |
| Project GPG | Optional residual | Explicitly not required to close gates |
| Full SLSA Build L3 | Not claimed | Reusable builder trust-boundary residual |

Do **not** invent signatures or provenance files to satisfy a checklist cell.

## Permanent compatibility fixture drops (post-first-tag)

| Drop | Path | Notes |
| --- | --- | --- |
| Codec N−1 | `tests/fixtures/released/<label>/` | Policy + `self-v1` harness exist; permanent tag trees absent |
| Store N−1 | `tests/fixtures/released-stores/<semver>/` | Empty of semver drops; fail-closed producer |
| ABI consumer | Release asset `glifistore-abi-v1-consumer-…tar.xz` | Packager/validator exist; old-binary×new-library row blocked |
| Wire client | Release asset sealed wire-v2 client | Same residual as ABI |

## Related

- [Release checklist](../../../docs/assurance/release-checklist.md)
- [Wave 5 L7 residuals](../../../docs/distribution/wave5-l7-residuals.md)
- [Artifact delivery sealing](../../../docs/distribution/artifact-delivery.md)
