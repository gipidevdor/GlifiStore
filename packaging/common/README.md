# Shared packaging assets

This directory holds the backend-independent packaging material, so that the Debian, RPM, MacPorts
and Homebrew packages and the BSD reference ports describe the same product instead of drifting
apart.

## Contents

| Path | Role | State |
| --- | --- | --- |
| [`config-data-policy.md`](config-data-policy.md) | Normative intent for retaining configuration and data across install, upgrade and removal | Resolved for deb and rpm; intent only for the other backends |
| [`file-lists/payload.yaml`](file-lists/payload.yaml) | The one installed-file inventory, split into components by retention policy | Used by the deb and rpm payload checks |
| [`service/`](service/) | systemd unit and service-account snippet shared by both Linux backends | Rendered and installed; installed disabled, never auto-started |
| [`consumer/`](consumer/) | External consumer sources that link against the installed package | Built outside the checkout by the `external-consumer` check |

## State

- The Linux backends render their packaging metadata from the release context and can walk the
  full lifecycle (build, install, external consumer, service, protocol round trip, restart
  recovery, configuration preservation, removal), but only inside the digest-pinned container or on
  a disposable root Linux host. Anywhere else those rows report `BLOCKED` with the reason. Nightly
  Package CI retains that evidence, and `GATE-PACKAGE-LIFECYCLE` (`IMPLEMENTATA`) cites it: run
  [34662210614](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614) reached
  `LIFECYCLE_VERIFIED` for deb and rpm (rpm on the earlier Fedora 41 pin), with `service-lifecycle`
  passing under systemd as PID 1. The overall result stays `NOT_RUN` without a sealed candidate,
  and `required_for_release` is `false`.
- The MacPorts port declares an unprivileged launchd startup item, and the Homebrew formula a
  `service do` block for `brew services` (templates under [`../macports/`](../macports/) and
  [`../homebrew/`](../homebrew/)). No native run is retained, so their `service-lifecycle` check
  stays an open gate.
- The FreeBSD and OpenBSD reference ports under [`../freebsd/`](../freebsd/) and
  [`../openbsd/`](../openbsd/) are not accepted by an upstream ports tree.
- Package versions are never written by hand. `engineering/tools/semver_policy.py` derives them from
  `VERSION` and the package revision and records them in the release context, as the mapping below
  shows. The metadata renderer refuses to read a template that contains a literal product version.

## Version mapping

`package_revision` in the release context is packaging-only: it changes when the package changes
without a product change, and it defaults to `0`.

| Backend | Version fields | `0.1.0`, revision `0` |
| --- | --- | --- |
| Debian | `upstream_version`, `debian_revision` | `0.1.0-1` |
| RPM | `version`, `release` | `0.1.0-1` |
| MacPorts | `version`, `revision` | `0.1.0_0` |
| Homebrew | `version`, `revision` | `0.1.0` |
| FreeBSD | `DISTVERSION`, `PORTREVISION` | `0.1.0` |
| OpenBSD | `V`, `REVISION` | `0.1.0` |

Debian and RPM map a prerelease with `~`, so that it sorts before the final release. MacPorts,
Homebrew and the BSD ports carry documented prerelease ordering limitations in the release context
instead of an unproven ordering claim.
