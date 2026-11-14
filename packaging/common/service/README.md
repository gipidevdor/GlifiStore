# Service integration

This directory holds the service definitions shared by the Linux packages: the Debian and RPM
backends install the same unit, rendered from one template, so the two packages cannot drift into
different runtime behavior.

| Path | Role |
| --- | --- |
| [`glifistored.service.in`](glifistored.service.in) | systemd unit template |
| [`service-account.sh.in`](service-account.sh.in) | Shell snippet that creates the `glifistore` system user and group, inlined into `postinst` (deb) and `%pre` (rpm) |

[`render_package_metadata.py`](../../../engineering/tools/render_package_metadata.py) renders both
files, substituting the layout tokens (`@BINDIR@`, `@STATEDIR@`, `@CONFIG_FILE@`, `@SERVICE_USER@`,
`@SERVICE_GROUP@`) for the backend being built. Neither file is installed from this directory
directly, and neither may be edited after rendering.

## What the unit commits to

- The daemon runs as the unprivileged `glifistore` system account, never as root.
- `StateDirectory=glifistore` gives systemd ownership of the durable directory's creation and mode
  (`0750`), so an install does not have to guess.
- `ProtectSystem=strict`, with an explicit `ReadWritePaths` for the state directory: the daemon
  cannot write anywhere else, including its own configuration.
- `TimeoutStopSec=90s` deliberately exceeds the daemon's `shutdown-drain-ms`, so systemd does not
  `SIGKILL` a daemon that is still draining. Shortening it would change durable shutdown behavior
  and needs an ADR.
- The unit is **installed but not enabled**: `dh_installsystemd --no-enable --no-start` on Debian,
  and no `%systemd_post` enablement on RPM. Installing a package never starts a storage daemon on an
  operator's machine.

## What is proven

Nothing here is certified. The `service-lifecycle`, `put-get-erase` and `restart-recovery` checks
exercise the unit (a start, `systemctl is-active`, a protocol round trip against the installed
daemon, a restart and a re-read of the data written before it), but only where the lifecycle can
run: inside the digest-pinned container or on a disposable root Linux host. Everywhere else those
checks report `BLOCKED` with the reason. `GATE-PACKAGE-LIFECYCLE` (`IMPLEMENTATA`) cites the
retained nightly Package CI evidence: run
[34662210614](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614) reached
`LIFECYCLE_VERIFIED` for both Linux backends, with `service-lifecycle` passing under systemd as
PID 1. The overall result stays `NOT_RUN` without a sealed candidate, and `required_for_release`
stays `false` for both.

The macOS backends declare their own service integration: the MacPorts port an unprivileged launchd
startup item, and the Homebrew formula a `service do` block for `brew services`. No native run is
retained, so their `service-lifecycle` check stays an open gate. The BSD reference ports keep their
own native scripts: the FreeBSD [rc.subr script](../../freebsd/files/glifistored.in) and the
OpenBSD [rc.d script](../../openbsd/pkg/glifistored.rc).
