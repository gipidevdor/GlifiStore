# MacPorts port

This directory holds upstream-submittable reference material for a MacPorts `databases/glifistore`
port. It is **not an accepted port**: no `glifistore` entry exists in the MacPorts ports tree, and
nothing in this repository may claim otherwise. The `upstream-ports-acceptance` check stays
`OPEN_GATE` until that changes.

## Contents

| Path | Role |
| --- | --- |
| [`Portfile.in`](Portfile.in) | Portfile template; carries no version, master site or checksum |
| [`files/glifistored.conf.sample`](files/glifistored.conf.sample) | Prefix-relative configuration sample, installed as `${prefix}/etc/glifistore/glifistored.conf.sample` |

[`render_macos_packaging.py`](../../engineering/tools/render_macos_packaging.py) produces a usable
`Portfile`, injecting the port version from the `VERSION` authority through the release context
and the distfile identity from the source archive it is given. There is no checked-in `Portfile`,
so a port version can never drift from `VERSION`.

## Render and lint

```bash
python3 engineering/tools/generate_release_context.py --output /tmp/ctx.json
python3 engineering/tools/render_macos_packaging.py --backend macports --profile pr \
  --release-context /tmp/ctx.json \
  --source-url "file:///path/to/GlifiStore-<version>.tar.xz" \
  --source-sha256 "<sha256>" --output-dir /tmp/port

mkdir -p /tmp/tree/databases/glifistore
cp /tmp/port/Portfile /tmp/tree/databases/glifistore/
cp -R packaging/macports/files /tmp/tree/databases/glifistore/
cd /tmp/tree/databases/glifistore && port lint --nitpick
```

`scripts/package-ci.sh --profile pr --backend macports` does the same and records the outcome as
package evidence.

## Prefix isolation

A MacPorts build must resolve everything inside its own prefix. The Portfile refuses `/opt/homebrew`
and `/usr/local` through `CMAKE_IGNORE_PATH` and `CMAKE_IGNORE_PREFIX_PATH`, and the
`prefix-isolation` check proves it again after installation with `otool -L` over the installed
daemon and shared library.

A link into a foreign prefix is a defect. The only way to accept one is to record it, with a reason,
in `packaging/macports/prefix-exceptions.txt` (`<install name> # <reason>`); no such file exists
today.

## Service integration

The port installs an unprivileged launchd startup item through the MacPorts 2.7+
`startupitem.user` and `startupitem.group` (`glifistore`). With `GLIFISTORE_PACKAGE_CI_NATIVE=1`,
the packaging lifecycle starts and stops it with `port load` and `port unload`. Without a retained
native run, `service-lifecycle` stays `OPEN_GATE`.

When the `put-get-erase` and `restart-recovery` checks run, they start the installed daemon
directly under a driver-owned configuration: a binary exercise, not a run of a service managed by
the init system.

## Upgrade continuity

`package-upgrade` stays `NOT_APPLICABLE_INITIAL_BASELINE` or `NOT_RUN` until a sealed SemVer
predecessor exists and sealed N−1 source archives are supplied through `GLIFISTORE_N1_PACKAGE_DIR`.
The native lifecycle then runs install→seed→upgrade→verify against those bytes, never rebuilt from
HEAD. Supplying the directory alone does not invent a PASS without a retained native run.

## Release sources

The release profile refuses anything that is not a sealed source archive: the URL must be `https`,
must be named `GlifiStore-<version>.tar.xz`, and must not look like a git ref or checkout tarball.
Set `GLIFISTORE_SOURCE_ARCHIVE_URL` and `GLIFISTORE_SOURCE_ARCHIVE_SHA256`, and optionally
`..._SIZE` and `..._RMD160`. For `pr`, `main` and `nightly`, the adapter may build from a `file://`
archive of `HEAD`, whose digest it verifies before pinning it.

## Native lifecycle

`port lint`, `port destroot`, `port install`, `port contents`, the external consumer build, the
daemon exercise, `port deactivate` and `port activate`, `port uninstall` and `port clean` run only
on a macOS host with MacPorts and `GLIFISTORE_PACKAGE_CI_NATIVE=1`, because they mutate the host
package manager. Everything that did not run is reported as `NOT_RUN` or `BLOCKED`, never as
`PASS`.
