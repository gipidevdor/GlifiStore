# Homebrew formula

This directory holds upstream-submittable reference material for a `glifistore` Homebrew formula.
The formula is **not in `homebrew/core`**, and there is no official GlifiStore tap; the
`upstream-ports-acceptance` check stays `OPEN_GATE` until one exists.

## Template

[`glifistore.rb.in`](glifistore.rb.in) is the formula template; it carries no version, url or
sha256. [`render_macos_packaging.py`](../../engineering/tools/render_macos_packaging.py) produces a
usable `glifistore.rb`, injecting the version from the `VERSION` authority through the release
context and the source identity from the archive it is given. Nothing is checked in already
rendered, so the formula version can never drift from `VERSION`.

## Render and audit

```bash
python3 engineering/tools/generate_release_context.py --output /tmp/ctx.json
python3 engineering/tools/render_macos_packaging.py --backend homebrew --profile main \
  --release-context /tmp/ctx.json \
  --source-url "file:///path/to/GlifiStore-<version>.tar.xz" \
  --source-sha256 "<sha256>" --output-dir /tmp/formula

brew audit --strict --formula /tmp/formula/glifistore.rb
```

`scripts/package-ci.sh --profile main --backend homebrew` does the same and records the outcome as
package evidence.

## Prefix isolation

A Homebrew build must not reach into a MacPorts prefix. The formula refuses `/opt/local` through
`CMAKE_IGNORE_PATH` and `CMAKE_IGNORE_PREFIX_PATH`, and the `prefix-isolation` check proves it again
after installation with `otool -L` over the installed daemon and shared library. `/usr/local` is
not forbidden here: on Intel macOS it is Homebrew's own prefix.

A link into `/opt/local` is a defect. The only way to accept one is to record it, with a reason, in
`packaging/homebrew/prefix-exceptions.txt` (`<install name> # <reason>`); no such file exists today.

## Service integration

The formula declares a `service do` block. With `GLIFISTORE_PACKAGE_CI_NATIVE=1`, the packaging
lifecycle starts, health-checks and stops GlifiStore through `brew services`. Without a retained
native run, `service-lifecycle` stays `OPEN_GATE` and the backend cannot report `PASS`.

When the `put-get-erase` and `restart-recovery` checks run, they start the installed daemon
directly under a driver-owned configuration: a binary exercise, not a run of a service managed by
the init system.

## Upgrade continuity

`package-upgrade` stays `NOT_APPLICABLE_INITIAL_BASELINE` or `NOT_RUN` until a sealed SemVer
predecessor exists and sealed N−1 source archives are supplied through `GLIFISTORE_N1_PACKAGE_DIR`.
The native lifecycle then runs install→seed→upgrade→verify against those bytes, never rebuilt from
HEAD. Supplying the directory alone does not invent a PASS without a retained native run.

## Configuration and data

`cmake --install` writes a sample whose `data-dir` points inside the versioned keg, so the formula
rewrites it to `#{var}/glifistore` and installs it as `#{etc}/glifistore/glifistored.conf.sample`.
Homebrew preserves `etc` files across upgrades, and `brew uninstall` never touches `var`, as the
shared [configuration and data retention policy](../common/config-data-policy.md) requires.

## Release sources

The release profile refuses anything that is not a sealed source archive: the URL must be `https`,
must be named `GlifiStore-<version>.tar.xz`, and must not look like a git ref or checkout tarball.
Set `GLIFISTORE_SOURCE_ARCHIVE_URL` and `GLIFISTORE_SOURCE_ARCHIVE_SHA256`. For `main` and
`nightly`, the adapter may build from a `file://` archive of `HEAD`, whose digest it verifies before
pinning it.

## Native lifecycle

`brew audit`, `brew install --build-from-source`, `brew list`, the external consumer build,
`brew test`, `brew services` start and stop, the daemon exercise, `brew uninstall` and
`brew cleanup` run only on a macOS host with Homebrew and `GLIFISTORE_PACKAGE_CI_NATIVE=1`, because
they mutate the host package manager. Everything that did not run is reported as `NOT_RUN` or
`BLOCKED`, never as `PASS`.
