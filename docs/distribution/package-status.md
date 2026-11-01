<!-- GENERATED FILE. Do not edit by hand.
     Authority: engineering/distribution/package-matrix.yaml
                engineering/tools/package_ci_plan.py (CI profile policy)
     Regenerate: python3 engineering/tools/generate_package_status.py --write-generated
-->

# Package backend status

> **Derived view.** The machine-readable authority is
> [`engineering/distribution/package-matrix.yaml`](../../engineering/distribution/package-matrix.yaml).
> A row below states what the matrix declares and how deep CI is allowed to go; it is never
> evidence that a package was built, installed, started or accepted upstream. The only
> packaging proofs are the retained `*-package-evidence.json` documents produced by
> [`scripts/package-ci.sh`](../../scripts/package-ci.sh) and validated by
> [`engineering/tools/validate_package_evidence.py`](../../engineering/tools/validate_package_evidence.py).
> GlifiStore remains an **architectural prototype**; no packaging gate is closed.

Operator guide: [package CI](package-ci.md) · Open residuals:
[Wave 5 (L7) residuals](wave5-l7-residuals.md).

## Backends

`status` is the repository-side maturity of the backend, `declared lifecycle state` is the
furthest point of the chain the matrix claims for it, and `required for release` says whether
[`release_bundle.validate_release_policy`](../../engineering/tools/release_bundle.py) demands
its artifact. A backend is promoted by a gate and an ADR, never by editing this page.

| Backend | Package kind | Status | Declared lifecycle state | Required for release | Wave | Profiles |
| --- | --- | --- | --- | --- | --- | --- |
| `deb` | `deb` | `STRUCTURAL` | `STRUCTURAL` | no | B | `main`, `nightly`, `pr`, `release` |
| `freebsd` | `freebsd_pkg` | `STRUCTURAL` | `STRUCTURAL` | yes | D | `main`, `nightly`, `pr`, `release` |
| `homebrew` | `homebrew_formula` | `STRUCTURAL` | `STRUCTURAL` | no | C | `main`, `nightly`, `release` |
| `macports` | `macports_port` | `STRUCTURAL` | `STRUCTURAL` | no | C | `main`, `nightly`, `pr`, `release` |
| `openbsd` | `openbsd_tgz` | `STRUCTURAL` | `STRUCTURAL` | yes | D | `main`, `nightly`, `pr`, `release` |
| `rpm` | `rpm` | `STRUCTURAL` | `STRUCTURAL` | no | B | `main`, `nightly`, `release` |

## Targets

A container target is pinned by digest; a host target runs on the runner itself.

| Target | Backend | Platform | Arch | Runner | Image | Digest | Profiles |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `deb-debian-12-amd64` | `deb` | `debian-12` | `amd64` | `ubuntu-24.04` | `docker.io/library/debian:12` | `sha256:6ebd97fa83de…` | `main`, `nightly`, `pr`, `release` |
| `deb-ubuntu-24.04-amd64` | `deb` | `ubuntu-24.04` | `amd64` | `ubuntu-24.04` | `docker.io/library/ubuntu:24.04` | `sha256:224a1869083a…` | `main`, `nightly`, `release` |
| `freebsd-14-amd64` | `freebsd` | `freebsd-14` | `amd64` | `ubuntu-latest` | — | — | `main`, `nightly`, `pr`, `release` |
| `homebrew-macos-15-arm64` | `homebrew` | `macos-15` | `arm64` | `macos-15` | — | — | `main`, `nightly`, `release` |
| `macports-macos-15-arm64` | `macports` | `macos-15` | `arm64` | `macos-15` | — | — | `main`, `nightly`, `pr`, `release` |
| `openbsd-7-amd64` | `openbsd` | `openbsd-7` | `amd64` | `ubuntu-latest` | — | — | `main`, `nightly`, `pr`, `release` |
| `rpm-fedora-43-amd64` | `rpm` | `fedora-43` | `amd64` | `ubuntu-24.04` | `quay.io/fedora/fedora:43` | `sha256:37d4208f1aa0…` | `main`, `nightly`, `release` |

## Required checks per profile

A backend that does not run in a profile shows —. Every other check the backend
declares still has to report an honest status; only the checks below may not be omitted.

| Backend | `pr` | `main` | `nightly` | `release` |
| --- | --- | --- | --- | --- |
| `deb` | `package-metadata-render`, `structural-metadata` | `package-metadata-render`, `structural-metadata` | `package-metadata-render`, `structural-metadata` | `package-metadata-render`, `sealed-source-admission`, `structural-metadata` |
| `freebsd` | `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `package-upgrade`, `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `config-preservation`, `package-build`, `package-inspect`, `package-install`, `package-remove`, `package-upgrade`, `ports-account-registration`, `put-get-erase`, `reference-port-structure`, `restart-recovery`, `sealed-source-admission`, `service-lifecycle`, `structural-metadata`, `upstream-ports-acceptance` |
| `homebrew` | — | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` |
| `macports` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` | `package-metadata-render`, `structural-metadata`, `upstream-ports-acceptance` |
| `openbsd` | `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `package-upgrade`, `ports-account-registration`, `reference-port-structure`, `structural-metadata`, `upstream-ports-acceptance` | `config-preservation`, `package-build`, `package-inspect`, `package-install`, `package-remove`, `package-upgrade`, `ports-account-registration`, `put-get-erase`, `reference-port-structure`, `restart-recovery`, `sealed-source-admission`, `service-lifecycle`, `structural-metadata`, `upstream-ports-acceptance` |
| `rpm` | — | `package-metadata-render`, `structural-metadata` | `package-metadata-render`, `structural-metadata` | `package-metadata-render`, `sealed-source-admission`, `structural-metadata` |

## Lifecycle check vocabulary

Evidence may only use these ids. The category bounds how a passing row may ever be read:
a `structural` pass is never a package, service or upstream-acceptance proof.

| Check | Stage | Category | What a pass means |
| --- | --- | --- | --- |
| `config-preservation` | `remove` | `package` | reinstall over an operator-modified configuration and prove it survives |
| `external-consumer` | `verify` | `package` | build an external consumer against the installed prefix without the source tree |
| `package-build` | `build` | `native-build` | build the native package from the sealed source archive |
| `package-inspect` | `inspect` | `package` | inspect package metadata, dependencies and binary hardening before installation |
| `package-install` | `install` | `package` | install the package on a clean target and prove the declared file inventory |
| `package-lint` | `metadata` | `structural` | run the native packaging linter or auditor over the rendered metadata |
| `package-metadata-render` | `metadata` | `structural` | render backend packaging metadata from the release context without a hard-coded version |
| `package-remove` | `remove` | `package` | remove the package under the config-vs-data retention policy |
| `package-upgrade` | `upgrade` | `package` | upgrade from the sealed N-1 package and prove dataset continuity (Linux deb/rpm, FreeBSD/OpenBSD, and MacPorts/Homebrew run install→seed→upgrade→verify when GLIFISTORE_N1_PACKAGE_DIR supplies sealed predecessor packages or macOS source archives; package bytes / source archives are never rebuilt from HEAD. MacPorts/Homebrew still render the Portfile/formula from tip templates against that sealed source digest until archive-owned recipes are retained) |
| `ports-account-registration` | `metadata` | `upstream-accepted` | read the upstream service-account allocation marker without ever creating it |
| `prefix-isolation` | `inspect` | `package` | prove the installed binaries link only inside their own package-manager prefix |
| `put-get-erase` | `verify` | `service` | protocol-v2 PUT, exact GET, ERASE and NOT_FOUND through the packaged service |
| `reference-port-structure` | `metadata` | `structural` | validate the in-repo reference port against VERSION, ABI_VERSION and service policy |
| `restart-recovery` | `verify` | `service` | restart the packaged service and recover the exact durable value |
| `sealed-source-admission` | `build` | `native-build` | admit the sealed candidate source archive by seal digest before any package build |
| `service-lifecycle` | `verify` | `service` | start, health-check and stop the packaged service through the platform init system |
| `structural-metadata` | `metadata` | `structural` | load the release context and package matrix and refuse an unknown backend |
| `upstream-ports-acceptance` | `metadata` | `upstream-accepted` | record whether an upstream ports tree or tap accepted the packaging |

## Lifecycle states and statuses

The lifecycle chain a backend may climb, in order:

`NONE` → `STRUCTURAL` → `BUILT` → `INSTALLED` → `FUNCTIONALLY_VERIFIED` → `LIFECYCLE_VERIFIED` → `UPGRADE_VERIFIED` → `RELEASE_ADMITTED`

A check reports exactly one status. `NOT_RUN`, `BLOCKED` and `OPEN_GATE` are first-class
outcomes and are never rounded up to `PASS`; [package-ci.md](package-ci.md) documents how
to read each one.

## CI profile policy

Owned by [`engineering/tools/package_ci_plan.py`](../../engineering/tools/package_ci_plan.py):
[`package-ci.yml`](../../.github/workflows/package-ci.yml) restates no profile, retention or
depth of its own. The `release` profile is only reachable through a call from
[`release.yml`](../../.github/workflows/release.yml) with sealed candidate bytes.

| Profile | Events | Sealed artifacts required | Evidence retention (days) | Lifecycle depth available |
| --- | --- | --- | --- | --- |
| `pr` | `pull_request`, `workflow_dispatch`, `workflow_call` | no | 7 | structural; opt-in native (`--allow-native`) for `macports` |
| `main` | `push`, `workflow_dispatch`, `workflow_call` | no | 30 | structural; opt-in native (`--allow-native`) for `homebrew`, `macports` |
| `nightly` | `schedule`, `workflow_dispatch`, `workflow_call` | no | 30 | structural; digest-pinned container for `deb`, `rpm`; opt-in native (`--allow-native`) for `homebrew`, `macports` |
| `release` | `workflow_call` | yes | 14 | structural; digest-pinned container for `deb`, `rpm`; opt-in native (`--allow-native`) for `homebrew`, `macports` |

## Backend limitations

Copied verbatim from the matrix; each one bounds what the backend may ever claim.

### `deb` — Debian / Ubuntu .deb

- packaging/debian/templates is rendered by engineering/tools/render_package_metadata.py; the build, install, service and removal rows only run inside the digest-pinned container (GLIFISTORE_PACKAGE_CI_CONTAINER=1) or on a disposable root host (GLIFISTORE_PACKAGE_CI_NATIVE=1). Every other environment reports them BLOCKED.
- .github/workflows/package-ci.yml retains this backend's evidence per profile. Retained nightly container runs reach LIFECYCLE_VERIFIED with service-lifecycle PASS under systemd as PID 1 (run 34662210614); overall result stays NOT_RUN without a sealed candidate. required_for_release stays false until a gate promotes this backend.
- package-upgrade selects the SemVer predecessor from the release context. When GLIFISTORE_N1_PACKAGE_DIR supplies sealed predecessor packages, the container lifecycle runs install→seed→upgrade→verify; otherwise the check stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN. N-1 is never rebuilt from HEAD.
- The library package is not Multi-Arch: same and installs into a non-multiarch libdir, because the installed glifistore-abi.pc derives its prefix two levels above itself.
- Only the executed Debian and Ubuntu rows may ever be reported; no other Debian derivative is claimed.

### `freebsd` — FreeBSD reference port and .pkg

- scripts/package-ci.sh --backend freebsd runs the native package/service lifecycle (scripts/test-freebsd-package-lifecycle.sh) only on a native FreeBSD host with the sealed candidate; every other host reports the native rows as BLOCKED.
- PORTS_ACCOUNT_REGISTERED is a real upstream UID/GID allocation and is never synthesised by CI.
- An in-repo reference port is the project packaging pipeline, not a FreeBSD ports-tree acceptance; upstream-ports-acceptance stays an open gate until upstream accepts it.
- package-upgrade stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN until sealed N-1 .pkg packages are supplied via GLIFISTORE_N1_PACKAGE_DIR; the native lifecycle then runs install→seed→upgrade→verify (never rebuilds N-1 from HEAD).

### `homebrew` — Homebrew Formula

- packaging/homebrew/glifistore.rb.in is rendered by engineering/tools/render_macos_packaging.py; the native audit, build, install and daemon rows only run on a macOS host with Homebrew and GLIFISTORE_PACKAGE_CI_NATIVE=1.
- The formula declares a brew services block; the native lifecycle starts, health-checks and stops it when GLIFISTORE_PACKAGE_CI_NATIVE=1. Without a retained native run, service-lifecycle stays an open gate and this backend cannot report PASS.
- package-upgrade stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN until sealed N-1 source archives are supplied via GLIFISTORE_N1_PACKAGE_DIR; the native lifecycle then runs install→seed→upgrade→verify. Sealed source bytes are never rebuilt from HEAD; the formula is still rendered from tip templates against that sealed digest until archive-owned packaging recipes are retained.
- In-repo packaging is the project pipeline; it is not an official tap acceptance claim.

### `macports` — MacPorts Portfile

- packaging/macports/Portfile.in is rendered by engineering/tools/render_macos_packaging.py; the native port, install and daemon rows only run on a macOS host with MacPorts and GLIFISTORE_PACKAGE_CI_NATIVE=1.
- The port declares an unprivileged launchd startup item (startupitem.user/group glifistore); the native lifecycle loads and unloads it when GLIFISTORE_PACKAGE_CI_NATIVE=1. Without a retained native run, service-lifecycle stays an open gate and this backend cannot report PASS.
- package-upgrade stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN until sealed N-1 source archives are supplied via GLIFISTORE_N1_PACKAGE_DIR; the native lifecycle then runs install→seed→upgrade→verify. Sealed source bytes are never rebuilt from HEAD; the Portfile is still rendered from tip templates against that sealed digest until archive-owned packaging recipes are retained.
- In-repo packaging is the project pipeline; it is not an upstream ports-tree acceptance claim.

### `openbsd` — OpenBSD reference port and .tgz

- scripts/package-ci.sh --backend openbsd runs the native package/service lifecycle (scripts/test-openbsd-package-lifecycle.sh) only on a native OpenBSD host with the sealed candidate; every other host reports the native rows as BLOCKED.
- LibreSSL from base remains the only supported TLS backend on OpenBSD.
- PORTS_ACCOUNT_REGISTERED is a real upstream UID/GID allocation and is never synthesised by CI.
- An in-repo reference port is the project packaging pipeline, not an OpenBSD ports-tree acceptance; upstream-ports-acceptance stays an open gate until upstream accepts it.
- package-upgrade stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN until sealed N-1 .tgz packages are supplied via GLIFISTORE_N1_PACKAGE_DIR; the native lifecycle then runs install→seed→upgrade→verify (never rebuilds N-1 from HEAD).

### `rpm` — Fedora / RPM

- packaging/rpm/templates/glifistore.spec.in is rendered by engineering/tools/render_package_metadata.py; the build, install, service and removal rows only run inside the digest-pinned container (GLIFISTORE_PACKAGE_CI_CONTAINER=1) or on a disposable root host (GLIFISTORE_PACKAGE_CI_NATIVE=1). Every other environment reports them BLOCKED.
- .github/workflows/package-ci.yml retains this backend's evidence per profile. Retained nightly container runs reach LIFECYCLE_VERIFIED with service-lifecycle PASS under systemd as PID 1 (run 34662210614 on the previous Fedora 41 pin); overall result stays NOT_RUN without a sealed candidate. The live target is digest-pinned Fedora 43 (Fedora 41 is EOL). required_for_release stays false until a gate promotes this backend.
- package-upgrade selects the SemVer predecessor from the release context. When GLIFISTORE_N1_PACKAGE_DIR supplies sealed predecessor packages, the container lifecycle runs install→seed→upgrade→verify; otherwise the check stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN. N-1 is never rebuilt from HEAD.
- RPM has no purge action, so erase is the terminal state for the configuration: the operator edit survives as the file itself or as a .rpmsave sibling.
- Rocky, Alma and RHEL are not claimed; only the executed Fedora row may ever be reported.

## Release policy artifacts

The artifact set
[`release_bundle.validate_release_policy`](../../engineering/tools/release_bundle.py) already
requires for every release. A backend outside this set cannot be admitted as a release
artifact, however deep its lifecycle ran.

| Artifact | Description | Required for release |
| --- | --- | --- |
| `abi_consumer` | ABI consumer fixture archive | yes |
| `freebsd` | native FreeBSD .pkg | yes |
| `linux` | Linux install prefix archive | yes |
| `openbsd` | native OpenBSD .tgz | yes |
| `source` | sealed source archive GlifiStore-&lt;version&gt;.tar.xz | yes |
| `wire_client` | wire client fixture archive | yes |

## Out of scope

Refused by [`generate_package_matrix.py`](../../engineering/tools/generate_package_matrix.py)
rather than merely unimplemented: adding one of these needs the stated prerequisites first.

| Target | Reason | Required before it may enter scope |
| --- | --- | --- |
| `apple-pkg` | Apple .pkg installers are deliberately not part of this architecture: no ADR, no Apple signing/notarization identity and no update model. macOS is served by MacPorts and Homebrew. | accepted ADR, signing identity and an update model |
| `msi` | Windows installer format; follows the Windows out-of-scope decision. | accepted ADR plus platform durability evidence rows |
| `windows` | Windows is not a supported GlifiStore platform; no runtime, durability or service model exists. | accepted ADR plus platform durability evidence rows |
