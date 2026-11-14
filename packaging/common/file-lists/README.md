# Shared installed-file inventory

[`payload.yaml`](payload.yaml) is the one inventory of what a GlifiStore package installs, split
into the components the retention policy cares about, so that a payload claim is checked against a
declared list rather than against whatever the build happened to produce.

## Components

| Component | Contents | Removed by `remove` | Removed by `purge` |
| --- | --- | --- | --- |
| `runtime` | daemon, operator tools, `share/GlifiStore` metadata | yes | yes |
| `library` | `libglifistore.so.<abi-major>` and its full-version sibling | yes | yes |
| `development` | headers, static libraries, the `.so` link, pkg-config and CMake package files | yes | yes |
| `documentation` | manual pages | yes | yes |
| `configuration` | `/etc/glifistore/glifistored.conf` | no | yes (deb); rpm has no purge |
| `service` | the systemd unit | yes | yes |
| `data` | the durable state directory | no | no |

**No package action ever removes `data`**, and `configuration` survives removal, so an operator's
edits outlive a reinstall. That split is the reason the inventory is divided into components; the
[configuration and data retention policy](../config-data-policy.md) states it.

## Tokens

Paths use lowercase layout tokens (`@bindir@`, `@libdir@`, `@sysconfdir@`, `@unitdir@`,
`@statedir@`, `@abi_major@`, …) because the Debian and RPM layouts differ, most visibly `/usr/lib`
against `/usr/lib64` and `/lib/systemd/system` against `/usr/lib/systemd/system`. The tokens resolve
from the `layout` block of the `rendered-metadata.json` that the metadata renderer emits, so the
inventory and the package it checks always agree by construction.

## Checker

[`verify_package_payload.py`](../../../engineering/tools/verify_package_payload.py) resolves the
tokens against a root and asserts each entry's declared kind: `file`, `executable`,
`shared_library`, `directory` or `glob`. `--component` names what must be present and
`--absent-component` what must be gone, so one tool serves both the install check and the removal
check. It refuses an invocation that asks for neither, and a component named in both lists.

The BSD reference ports keep their native `pkg-plist` and `PLIST` as their own authority; this
inventory covers the Debian and RPM backends.
