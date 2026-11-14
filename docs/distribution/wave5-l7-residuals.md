# Release residuals

**Descriptive** · N↔N−1 fixtures, packages, signing/SLSA/SBOM, sealed publish · reviewed 2026-09-12

The release, portability and supply-chain work that still lacks retained tagged evidence, and what
would close each item: C ABI cross-release, wire and disk N↔N−1 fixtures, BSD packages, the
multi-backend packaging framework, signing, SLSA, SBOM and sealed publish. This ledger is not a gate
closure. Producers and validators may exist without retained tagged evidence; **absence of evidence
is a hard residual, not a waiver**. The claim ceiling stays architectural prototype.

## C ABI v1 cross-release fixtures

| Layer | Status |
| --- | --- |
| Specification, ADR 0038 and `ABI_VERSION` | Present |
| Symbol allowlist, layout probes, installed pure-C consumer | Present (`ci.yml` `install-consumer`) |
| Deterministic consumer packager, fixture schema and validator | Present |
| Same-run `abi-compatibility-evidence` producer | Present; fail-closed without a prior attested release |
| Permanent old-binary × new-library row from a real tag | Blocked: no complete official ABI-1 release asset retained yet |

First-tag procedure (do not invent digests):

1. Cut the annotated `vX.Y.Z` only when the other release prerequisites are closed or intentional
   residuals.
2. The candidate packages `glifistore-abi-v1-consumer-…tar.xz` via
   `scripts/package-abi-consumer.py`.
3. Retain that asset on the GitHub Release; optionally record a pointer under
   `engineering/evidence/release/` (never a hand-written SHA substitute).
4. The next tag's `abi-compatibility-evidence` job selects that prior release and runs both library
   directions through `scripts/test-installed-abi-compat.sh`.

Until step 4 succeeds with retained logs, matrix row `ABI-C1-N-MINUS-1` stays `not_promised`.

## Wire and disk N↔N−1 fixtures

| Kind | Present | Residual |
| --- | --- | --- |
| In-tree golden codecs | `tests/fixtures/*.hex` | Not cross-release proof |
| Released codec harness | `tests/fixtures/released/self-v1/` + `release-compat.yml` | Permanent `<semver>/` trees absent |
| Complete Store drops | `tests/fixtures/released-stores/` (README only) | No `<semver>/` baseline |
| Wire sealed client producer | `package-wire-client.py` + release job | No prior attested client/server bytes |

Fail-closed producers refuse same-build substitutes, as recorded in
[`n-n1-matrix.yaml`](../../engineering/compatibility/n-n1-matrix.yaml) and
[retained release evidence](release-evidence.md).

## FreeBSD and OpenBSD

| Signal | What it proves | What it does not prove |
| --- | --- | --- |
| `freebsd.yml` / `openbsd-libressl.yml` | Native build and tests (plus ABI install smoke) | Ports/pkg lifecycle |
| `packaging/{freebsd,openbsd}/` | Structural reference ports | Official ports acceptance or package bytes |
| `scripts/test-freebsd-package-lifecycle.sh` | Same-run producer (blocked on account markers) | Retained tagged package evidence |
| `scripts/test-openbsd-package-lifecycle.sh` | Same-run producer (blocked on account markers) | Retained tagged package evidence |
| `package-ci.sh --backend freebsd\|openbsd` | Structural rows plus a fail-closed hand-off to the native producers | Any native build, package, service or upstream row on a non-BSD runner |

`PORTS_ACCOUNT_REGISTERED` markers are deliberately absent until upstream ports allocation exists.
Do not commit a circular `distinfo` into the source archive.

BSD package evidence separates `structural`, `native-build`, `package`, `service` and
`upstream-accepted` rows ([BSD reference packaging](bsd-packaging.md)). Open by construction:

- `upstream-ports-acceptance` stays an open gate, so neither BSD backend can ever report `PASS`.
- `package-upgrade` is `NOT_APPLICABLE_INITIAL_BASELINE` (no prior annotated release) and becomes
  `NOT_RUN`, never `PASS`, until a sealed N−1 package artifact is admitted and the backend's
  install→seed→upgrade→verify walk succeeds. Linux deb/rpm, FreeBSD/OpenBSD and MacPorts/Homebrew
  implement that walk when `GLIFISTORE_N1_PACKAGE_DIR` is supplied.
- `external-consumer` against the installed package prefix is built by
  `scripts/lib/package-external-consumer.sh` inside the native FreeBSD/OpenBSD lifecycle scripts; a
  retained `PASS` lifts the BSD package-ci ceiling to `LIFECYCLE_VERIFIED`, while upgrade and
  upstream acceptance remain open.
- `package-ci.yml` retains the structural BSD rows per profile, but the native producers still run
  only in the release workflow VM. `GATE-PACKAGE-LIFECYCLE` cites that workflow and stays
  `IMPLEMENTATA`: citing a retention path is not the same as having retained a run.

## Linux and macOS packages

The Linux container lifecycle (`deb`, `rpm`) is enabled from the `nightly` and `release` profiles
only; `main` stays structural. Retained nightly
[`34662210614`](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614) on tip
`58167e8` reaches `LIFECYCLE_VERIFIED` for deb (Debian 12) and rpm (Fedora 41 at the time; the live
rpm target is now Fedora 43): `service-lifecycle` `PASS` through systemd as PID 1, plus install,
protocol, restart, config preservation and removal. The overall result stays `NOT_RUN` on unsealed
nightly checkouts because `sealed-source-admission` is `NOT_RUN`, so there is no publishable claim.

How the container lifecycle runs:

- Container dispatch tries systemd-as-PID-1 create variants (`cgroupns=host`, then `private` +
  `docker.slice`) via `linux-systemd-pid1.sh`.
- After install, the lifecycle emits `installed-sdk-matrix.json` for package admission: an honest
  `NOT_RUN` with `package_installed: true` when sealed SDK archives are absent. Nightly and release
  container rows package the language SDK archives on the host and install the matrix toolchains
  inside the image, so a `PASS` is reachable.
- When the inner driver writes evidence and exits non-zero, the outer dispatcher prefers that report
  over inventing `BLOCKED`.
- Deb rules use Unix Makefiles with a static core. RPM `%cmake` forces `-DBUILD_SHARED_LIBS=OFF`,
  and install clears the container's `tsflags=nodocs` so manuals remain in the inventory.

MacPorts and Homebrew have no hosted runner that may install into the host package manager, so their
native rows stay opt-in (`--allow-native`, never on by default) and unproven.

## Packaging framework

Declarative status is generated into [package backend status](package-status.md); the operator path
is the [package CI operator guide](package-ci.md). Gates: `GATE-PACKAGE-LIFECYCLE` and
`GATE-PACKAGE-ADMISSION`, both `IMPLEMENTATA`. Requirements: `GS-RELEASE-PACKAGE-001`,
`GS-RELEASE-UPGRADE-001`. Hazard: `HAZ-034`.

| Residual | State today | What would close it |
| --- | --- | --- |
| deb / rpm container lifecycle | Retained nightly [`34662210614`](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614) (`58167e8`): deb+rpm `LIFECYCLE_VERIFIED` with `service-lifecycle` PASS under systemd PID 1; overall `NOT_RUN` without a sealed candidate | A sealed-candidate release-profile row that keeps `LIFECYCLE_VERIFIED` and admits publishable bytes |
| MacPorts `launchd` startup item | Portfile declares unprivileged `startupitem.user`/`group`; native lifecycle loads/unloads it; without a retained native run it stays `OPEN_GATE` | A retained native run that exercises `port load`/`unload` |
| Homebrew `brew services` | Native lifecycle starts/stops the declared block; without a retained native run it stays `OPEN_GATE` | A retained native run that exercises the services block |
| MacPorts / Homebrew native rows | Opt-in only (`--allow-native`); no hosted runner may install into the host package manager | A disposable macOS host or an accepted runner policy, with retained logs |
| `external-consumer` on BSD | Implemented in native lifecycle scripts; maps to package-ci `PASS` → `LIFECYCLE_VERIFIED` when the log is retained | A tagged/native retained run that keeps `*-external-consumer.log` |
| `package-upgrade` anywhere | Selection is wired: `NOT_APPLICABLE_INITIAL_BASELINE` today; once a predecessor exists, `NOT_RUN` until sealed packages are supplied via `GLIFISTORE_N1_PACKAGE_DIR` (never rebuilt from HEAD). Linux deb/rpm, FreeBSD/OpenBSD, and MacPorts/Homebrew implement install→seed→upgrade→verify when those bytes are available (macOS selects sealed `GlifiStore-<version>.tar.xz` sources) | A sealed N−1 package artifact retained beside a published release, plus a PASS on a required backend walk |
| Admission tools | `run_package_admission.py` is wired into `package-ci.yml` (sealed candidate) and `release.yml` (`package-admission` job); Linux nightly now retains cross-SDK PASS | A positive `admitted: true` after sealed N−1 `package-upgrade` on required backends |
| Cross-SDK post-install matrix | Retained nightly [`34666166603`](https://github.com/gipidevdor/GlifiStore/actions/runs/34666166603) (`ebcf1fa`): deb (Debian 12 + Ubuntu 24.04) and rpm (Fedora 41 then; live target Fedora 43) `installed-sdk-matrix.json` PASS against package-owned `/usr/bin/glifistored` (cpp/python/go/perl/ruby/erlang, plain) | Wire that retained PASS into a sealed-candidate admission report (`admitted: true` still needs N−1 `package-upgrade`) |
| Optional backends | `deb`, `rpm`, `macports`, `homebrew` are `required_for_release: false` and cannot admit a release artifact | Retained `LIFECYCLE_VERIFIED` evidence, a release-policy artifact, an ADR and a gate update |
| Upstream acceptance | `OPEN_GATE` for FreeBSD, OpenBSD, MacPorts and Homebrew; in-repo packaging is the project pipeline only | Actual acceptance by the upstream ports tree or tap |
| Apple `.pkg` | Deliberately out of scope; refused by the matrix validator | An accepted ADR, an Apple signing/notarization identity and an update model |
| Windows / MSI | Out of scope; refused by the matrix validator | An accepted ADR plus platform durability evidence rows |
| Package evidence retention | No annotated tag exists, so no packaging evidence has ever been retained from a release | The first complete tagged release run |

Until real runs exist,
[`engineering/evidence/release/`](../../engineering/evidence/release/README.md) and the
per-filesystem trees under
[`engineering/evidence/platform-durability/`](../../engineering/evidence/platform-durability/README.md)
are empty by design and carry READMEs only. An empty fixture directory is a residual, never an
implicit pass.

## Supply chain and sealed publish

- Actions: 40-character SHA pins (`validate_actions_pins.py`), including a Dependabot
  SHA-discipline check against `.github/dependabot.yml`.
- Publish (`release.yml`) re-verifies the seal, checksums and attestation, refuses an existing
  release tag, never rebuilds and never `--clobber`s assets: different bytes require a new version.
- Least privilege: candidate and verify default to `contents: read`; publish alone gets
  `contents: write`, behind the protected `release` Environment; signing jobs elevate `id-token` /
  `attestations` only when needed.
- Overlapping pull request and tag scans (Trivy/gitleaks in `supply-chain-scan.yml` and the tag
  `security-supply-chain`) are intentional: the pull request regression gate is not tag-retained
  matrix evidence. Do not prune the tag path while `security-matrix-evidence` requires those logs.

## Candidate build profile

The release candidate is built with `-DGLIFISTORE_ENABLE_TLS=OFF` and records `--tls-backend none`
in its build metadata ([`release-candidate.yml`](../../.github/workflows/release-candidate.yml)).
Every artifact derived from that candidate (source archive, Linux prefix, ABI and wire fixtures, and
any package built from the sealed source) is therefore a TLS-less build. No release artifact or
package may be described as providing transport security, and a future TLS-enabled candidate is a
different artifact set, not a relabelling of this one.

## Signing, SLSA and SBOM

The implemented paths (Cosign keyless, SPDX bind-sbom, optional GitHub attestations) are not
retained tagged evidence until a successful tag deposits artifacts and pointers under
[`engineering/evidence/release/`](../../engineering/evidence/release/README.md). Project GPG and
full SLSA Build L3 remain explicit residuals.

## Correctness gates and performance gates

Correctness, portability and distribution gates (`CI`, `Assurance`, FreeBSD/OpenBSD,
install-consumer, release sealing) stay independent of absolute performance budgets and hard-pinned
scaling ([evidence taxonomy](../assurance/evidence-taxonomy.md)). Hardware work toward
`ACCETTATA_PER_RILASCIO` does not reopen these sealing residuals, and release sealing must not
absorb absolute performance claims from macOS `local` benches.

## Related

- [Release checklist](../assurance/release-checklist.md)
- [Artifact delivery](artifact-delivery.md)
- [Package CI operator guide](package-ci.md)
- [Package backend status](package-status.md)
- [BSD reference packaging](bsd-packaging.md)
