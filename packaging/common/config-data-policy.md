# Configuration and data retention policy

The retention intent shared by every packaging backend and, for the Debian and RPM backends, the
behavior that their `config-preservation` and `package-remove` checks exercise.

Those checks run only where a package can be installed and removed: inside the digest-pinned
container or on a disposable root Linux host. Everywhere else they report `BLOCKED`. Nightly Package
CI run [34662210614](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614) reached
`LIFECYCLE_VERIFIED` for deb and rpm, a state that requires both checks to pass;
`GATE-PACKAGE-LIFECYCLE` (`IMPLEMENTATA`) cites it. The overall result stays `NOT_RUN` without a
sealed candidate, so that run is not release evidence for the policy below. The BSD and macOS
backends have not executed it at all.

## Categories

| Category | Example | Install | Upgrade | Remove | Purge |
| --- | --- | --- | --- | --- | --- |
| Program files | `bin/glifistored`, `lib/libglifistore.so.*` | installed | replaced | removed | removed |
| Configuration | `glifistored.conf` | installed from the sample; an existing file is never overwritten | operator edits preserved | preserved | removed, and the now-empty `/etc/glifistore` with it |
| Durable data | Store directory (`/var/lib/glifistore` on Linux, `/var/db/glifistore` on FreeBSD) | created, owned by the service account | untouched | preserved | never removed by a package action |
| Logs and runtime state | pid files, sockets | created at runtime | untouched | removed if empty | removed if empty |

## Rules

1. A package action must never destroy durable Store data. Data removal is an explicit operator
   action, not a side effect of `remove` or `purge`.
2. Configuration modified by the operator survives reinstall and upgrade. Each backend expresses
   this with its native mechanism: `@sample` on the BSD ports, `conffiles` on Debian,
   `%config(noreplace)` on RPM.
3. The service account owns the data directory; packages must not widen its permissions. The
   account itself is never deleted, because files elsewhere on the system may still be owned by it.
4. Removing a package must not leave a running daemon. The package stops the service; it does not
   ask the operator to.

## Per-backend resolution

`apt purge` removes the configuration and the now-empty `/etc/glifistore`, and leaves
`/var/lib/glifistore` and the service account alone. `dpkg` marks `glifistored.conf` as a conffile,
so an operator edit produces a prompt rather than a silent overwrite. The lifecycle's reinstall
states the policy explicitly with `--force-confdef --force-confold`: the operator's version wins.

RPM has no purge action, so removal is the terminal state for the configuration.
`%config(noreplace)` leaves an edited file in place or renames it to `.rpmsave`; either way the edit
survives. The durable directory is `%dir`-owned but never deleted.

Stopping the service on removal goes through the unit's `TimeoutStopSec=90s`, which outwaits the
daemon's shutdown drain. Neither backend shortens it, so removal does not truncate a durable
shutdown.
