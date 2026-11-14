# Command-line interface

**Descriptive** · `glifistored` and the `glifistore_*` tools

The options, configuration sources, signals, probes and exit codes of the GlifiStore programs. Every
program uses one strict parser and the same operational contract.

Installed builds ship portable [mdoc(7)](https://man.openbsd.org/mdoc.7) manuals under
`${CMAKE_INSTALL_MANDIR}`, described in [`man/README.md`](../man/README.md):

| Manual | Program |
| --- | --- |
| `man 8 glifistored` | network daemon |
| `man 7 glifistore` | overview |
| `man 1 glifistore_*` | offline tools and `glifistore_demo` |

## Conventions

- `-h` and `--help` print help to standard output and exit successfully.
- `-V` and `--version` print the executable name and the GlifiStore version.
- A long option takes its value as `--option value` or `--option=value`; selected common options
  also have short forms.
- An option may appear once unless its help says otherwise. A duplicate option is an error.
- `--` ends option parsing, so a path may begin with `-`.
- Invalid syntax exits with code 2 and writes the error and a help hint to standard error.
- A runtime or operating-system failure exits with code 1; success exits with code 0.
- Byte-size options take bytes or a `KB`, `KiB`, `MB`, `MiB`, `GB` or `GiB` suffix. Decimal and
  binary suffixes are deliberately distinct.

## Server

```bash
glifistored --bind 127.0.0.1 --port 7379 --shard-pairs 4
glifistored --config /etc/glifistore/daemon.conf
glifistored --profile embedded --data-dir /var/lib/glifistore --dump-config
glifistored --port 0 --shard-pairs 2 --max-input-bytes 4MiB --max-output-bytes 4MiB
glifistored --help
```

The [durable daemon guide](operations/durable-tcp-daemon.md) covers durable deployments: profile and
mode selection, resource flags, `HEALTH`/`READY`/`STATS`, drain and offline backup.

`--shard-pairs COUNT` is the canonical setting for the number of Reader/Writer shard pairs
([ADR 0031](adr/paired-reader-writer-shards.md)). `--workers COUNT` (`-w`) is a deprecated 0.1.x
alias for the same value; naming both flags fails closed. Wire and Manifest still report that pair
count as `worker_count`.

### Configuration precedence

Settings resolve as defaults < deployment profile < config file < environment < CLI. Unknown keys,
unknown profile names, empty values, duplicate keys in one file and conflicting `--reuse-port` /
`--no-reuse-port` (or their environment or file equivalents) fail closed before the process listens.

`--profile NAME`, `GLIFISTORE_PROFILE=NAME` or `profile = NAME` in a config file selects a built-in
deployment preset. The selection follows the same file, environment and CLI precedence as any other
setting, and the preset's values sit between the hardcoded defaults and the file, environment and
CLI overrides.

| Profile | Preset |
| --- | --- |
| `dev` | Volatile storage, background maintenance disabled; for local iteration |
| `embedded` | `durable-periodic` with constrained resource caps for sidecar or single-node embedded deployments; normal compaction pacing starts at 64 MiB/s |
| `production` | `durable-periodic` with background maintenance and the standard durable resource defaults; normal compaction pacing starts at 128 MiB/s; the operator must still supply `--data-dir` (or an equivalent) before listen |

Both pacing rates are overrideable starting points, not device-independent performance claims.

- `--config PATH` or `GLIFISTORE_CONFIG=PATH` selects a settings file. The file cannot set `config`,
  `help` or `version`.
- File keys are the long option names without `--` (`port = 7379`, `storage-mode = durable-sync`,
  `quiet = true`). Lines may be blank or start with `#`. Values may be quoted with `"..."`.
- Environment variables are `GLIFISTORE_` plus the long name in `SCREAMING_SNAKE_CASE`
  (`GLIFISTORE_PORT`, `GLIFISTORE_DATA_DIR`, `GLIFISTORE_MAX_STORE_BYTES`). Boolean flags accept
  `true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0`.
- `--dump-config` applies the same validation as before listen, prints the resolved effective
  settings as `GlifiStore/config` ASCII `key=value` lines, including the selected deployment
  profile, and exits without binding or opening a Store. TLS settings appear as paths only. The flag
  is CLI-only: neither a config file nor the environment can set it.

### Shutdown

`SIGINT` and `SIGTERM` request an orderly stop. The daemon stops accepting new connections, drains
existing connections and admitted durable mutations, then closes the Store. `--shutdown-drain-ms`
bounds the drain: 30 s by default; `0` waits without bound.

Idle connections close once their in-flight responses flush, and draining connections refuse new
requests. A queued mutation that has not entered Store execution when the deadline expires completes
as wire `OVERLOADED` (known not committed); in-flight Store work is never cancelled. A timed-out
drain makes process exit fail closed (`join` returns an error).

### Probes

`HEALTH` (opcode 7) and `READY` (opcode 8) are accepted before `INIT`/`BIND_WORKER`.

| Probe | Answer | Condition |
| --- | --- | --- |
| `HEALTH` (7) | `OK` with value `GlifiStore/live` | Executors are live |
| `READY` (8) | `OK` with value `GlifiStore/ready` | The Store is operational and maintenance is neither in emergency nor in a sticky faulted state |
| `STATS` (9) | A bounded ASCII admin report | The daemon is live |

A failed probe returns `INTERNAL_ERROR`. The `STATS` report carries the version, live and ready
state, connections, durable lane and batch counters, a maintenance snapshot (including the skip
reason and the no-gain planning scan counters) and durable rotation wait and execution phase
timings: seal, Segment creation, Manifest publication and final Record commit.

### Logging

`--quiet` suppresses the normal startup and shutdown messages, never errors.

Structured lifecycle logging is opt-in: `--log-format json`, `log-format = json` in a config file,
or `GLIFISTORE_LOG_FORMAT=json`. The default, `human`, keeps the legacy one-line startup and
shutdown messages on standard output and errors on standard error. JSON mode writes one object per
line on standard error, with these stable `event` names: `start`, `listen`, `ready`,
`maintenance_emergency`, `maintenance_fault`, `shutdown_begin`, `shutdown_drain_begin`,
`shutdown_drain_end`, `stopped` and `executor_failure`.

JSON fields are bounded (256-byte string cap) and omit TLS paths and other secrets. They carry only
the bind address, ports, executor count, storage mode, readiness reason, maintenance pressure and
state, and sanitized error codes and messages. `--quiet` suppresses the normal `start`, `listen` and
`stopped` events but never readiness loss, maintenance emergency or fault, shutdown drain or error
events.

### Resource limits

Server options bound connection counts, handoff queues, event batches and per-connection input and
output buffers. Unsupported shard-pair counts and buffers smaller than the protocol headers are
rejected before the server binds a socket.

Durable deployments add batch and resource caps: `--sync-interval-ms`, `--group-max-records`,
`--group-max-bytes`, `--group-max-wait-ms`, `--max-store-bytes`, `--reserved-free-bytes`,
`--max-segments`, `--max-hot-cache-bytes` and `--max-temporary-compaction-bytes`. These flags
require a durable `--storage-mode` and are validated before the process listens. The three hot-cache
options remain accepted for 0.1.x configuration compatibility, but the paired-only daemon disables
that legacy cache and reads through immutable `ReadGeneration` state.

`--reuse-port` and `--no-reuse-port` are mutually exclusive; the operating system decides whether
per-Reader listeners are available. `--executor-affinity` is strict on supported Linux systems and
advisory on macOS.

The daemon also accepts these bounded controls:

| Option | Meaning |
| --- | --- |
| `--max-connections` | process-wide admitted connection cap |
| `--handoff-capacity` | bounded one-time connection handoff queue per Reader |
| `--event-batch-size` | maximum readiness events processed per poll turn |
| `--durable-mutation-queue-capacity` | admitted mutation-record slots per shard pair |
| `--durable-mutation-queue-bytes` | owned mutation payload bytes per shard pair |
| `--durable-mutation-queue-wait-ms` | pre-Store queue-wait deadline; zero disables expiry |
| `--open-mode` | `open-or-create`, `create-new` or `open-existing` for durable storage |
| `--maintenance-mode` | `cooperative`, `background` or `disabled` scheduling |
| `--maintenance-unread-ttl-pressure-probe` | bounded unread-expiration probe under pressure or emergency |
| `--maintenance-unread-ttl-normal-scheduling` | opt in to unread-expiration probing for normal reclaim |
| `--max-hot-cache-value-bytes` | retained legacy-cache value limit; the paired daemon cache is disabled |
| `--disable-hot-cache` | explicitly disable the already inactive paired-daemon legacy cache |

### Maintenance limits

`--maintenance-max-copy-bytes-per-cycle` bounds the exact Index-referenced live Record bytes of one
normal background compaction. The daemon default is `128MiB`. A candidate exactly at the limit is
allowed, `0` explicitly removes the limit, and pressure or emergency bypass it to recover capacity.
The config file key is `maintenance-max-copy-bytes-per-cycle`; the environment variable is
`GLIFISTORE_MAINTENANCE_MAX_COPY_BYTES_PER_CYCLE`.

Maintenance rate budgets are distinct from the Phase 5 connection rate limits and from E3
power-loss certification:

```bash
glifistored ... --maintenance-max-copy-bytes-per-sec 1048576 \
  --maintenance-max-cpu-ms-per-window 25 \
  --maintenance-suspend-on-p99-latency-ms 40 \
  --maintenance-suspend-on-p99-min-samples 32 \
  --maintenance-max-latency-deferral-ms 30000
```

The copy rate spaces bounded physical writes to private pre-intent output. It is not a whole-job
size cap, so larger compactions still progress. Zero disables each byte, CPU and p99 threshold
budget. The latency guard resumes below 80% of its threshold, and its deferral bound admits one
reclaim attempt. Pressure and emergency bypass every normal fairness control.

### TLS

When built with LibreSSL or OpenSSL (`GLIFISTORE_ENABLE_TLS`), the daemon can wrap protocol v2 in
TLS 1.3 as an optional outer transport ([ADR 0020](adr/0020-tls-outer-transport.md),
[secure profile](security/secure-profile.md)):

```bash
# TLS-only on --port
glifistored --bind 127.0.0.1 --port 7379 \
  --tls-cert /etc/glifistore/server.crt --tls-key /etc/glifistore/server.key

# Dual: cleartext on --port, TLS on --tls-port
glifistored --bind 127.0.0.1 --port 7379 --tls-port 7380 \
  --tls-cert /etc/glifistore/server.crt --tls-key /etc/glifistore/server.key

# mTLS + authz (principal URI SAN → DNS SAN → CN; --authz-map default-deny)
glifistored ... --tls-cert ... --tls-key ... --tls-client-ca /etc/glifistore/clients-ca.crt \
  --authz-map /etc/glifistore/authz.map

# Fail-closed secure profile (TLS-only on --port; refuses --tls-port dual cleartext;
# applies Phase 5 abuse-limit defaults)
glifistored --secure-profile --bind 127.0.0.1 --port 7379 \
  --tls-cert ... --tls-key ... --tls-client-ca ... --authz-map ...

# Optional UDS alongside TCP (ADR 0029); peercred principal unix:uid=N for --authz-map
glifistored --unix-socket /var/run/glifistore.sock --unix-peercred --authz-map ...
```

`--tls-cert` and `--tls-key` are both required when any TLS path is set. Without `--tls-port`, TLS
makes `--port` TLS-only. With `--tls-port`, cleartext and TLS use distinct ports, and a collision
fails closed. Cleartext remains the default when the TLS flags are omitted. TLS configuration errors
fail before the process begins serving.

`--secure-profile` requires TLS, mTLS and `--authz-map`, and refuses `--tls-port`. It applies the
Phase 5 abuse defaults (an explicit `0` is refused), randomizes the Index mix seed unless
`--index-hash-seed` is set ([ADR 0026](adr/0026-keyed-index-hash-seed.md)) and randomizes Worker
routing unless `--worker-hash-seed` is set ([ADR 0030](adr/0030-keyed-worker-routing.md)). With
`--unix-socket`, it also requires `--unix-peercred`. UDS is not a TLS replacement
([ADR 0029](adr/0029-uds-peercred.md)).

`--secure-profile` also selects keyed SipHash Worker routing. The daemon and the official SDKs (C++,
Python, Perl, Go, Erlang, Ruby) implement the extended wire-v2 `INIT` identity. Those SDKs can use
explicitly configured TLS, mTLS and authz without `--secure-profile` while default FNV routing stays
active. Read the [SDK roadmap](architecture/sdk-roadmap.md) before choosing a client for the
complete profile.

Capability `admin` implies `write`, which implies `read`. Prefix-scoped principals need `admin` for
`STATS` ([ADR 0027](adr/0027-stats-isolation-prefix-principals.md)).

`--tls-crl PATH` loads a PEM CRL for mTLS revocation and requires `--tls-client-ca`.
`--tls-ocsp-fail-closed` requires that CRL-backed revocation configuration; it performs no live
AIA/HTTP OCSP.

### Phase 5 abuse controls

Abuse controls are distinct from maintenance rate budgets. Zero disables each limit and is the
trusted cleartext default.

```bash
glifistored ... \
  --max-accepts-per-sec 128 \
  --idle-timeout-ms 60000 \
  --request-timeout-ms 30000 \
  --connection-max-requests-per-sec 256 \
  --principal-max-requests-per-sec 1024 \
  --principal-max-bytes-per-sec 32MiB
```

An accept flood drops the peer; an exceeded request or bandwidth quota returns wire `overloaded`.
`HEALTH`, `READY` and `STATS` are exempt from the request and bandwidth quotas. `STATS` exports the
`abuse_*` counters. `--request-timeout-ms` never cancels a Store mutation already in execution.

## Maintenance tools

```bash
glifistore_inspect_segment [--json] [--no-scan] -- segment-<16hex>-<8hex>.glifi
glifistore_verify_store [--json] [--no-scan] -- /path/to/data-dir
glifistore_backup_store [--json] [--no-scan] -- /path/to/source /path/to/destination
glifistore_migrate_store [--json] [--no-scan] --workers N -- /path/to/source /path/to/destination
glifistore_repair_store [--json] [--no-scan] -- /path/to/source /path/to/empty-workspace
glifistore_rebuild_index -- segment-<16hex>-<8hex>.glifi
```

### `glifistore_inspect_segment`

Read-only validation of one durable Segment file. It takes no Store directory lock and never
modifies the file. By default it:

1. opens the file as a private, singly linked, owner-only regular file of exactly 64 MiB;
2. decodes the Segment header and selects the newest valid commit slot;
3. when the basename is a canonical `segment-…glifi` name, requires it to match the header identity;
4. scans the committed Record extent: CRC32C, sequences and agreement with the commit metadata.

`--no-scan` stops after header and commit validation. `--json` writes one stable single-line JSON
object to standard output. Exit codes: `0` validated, `1` validation or I/O failure, `2` usage.

A concurrent writer can make this point-in-time read observe a torn commit slot. The tool reports
that as a validation failure (fail closed), never as success.

### `glifistore_verify_store`

```bash
glifistore_verify_store [--json] [--no-scan] -- /path/to/data-dir
```

Read-only structural validation of a durable Store data directory. It takes the exclusive Store lock
and fails closed if a daemon or Store already holds it, so stop the daemon before verifying. By
default it:

1. exclusive-locks the data directory;
2. decodes `manifest.glifi` and enforces the Manifest resource bounds;
3. audits the namespace: missing, unlisted and unsafe entries fail, and crash temporaries may be
   reported;
4. opens each catalog Segment read-only against the Manifest identity;
5. checks Manifest role against commit lifecycle; sealed-active is accepted and counted as
   `active_requires_rotation`;
6. scans each committed Record extent unless `--no-scan` is given.

It does not rebuild Indexes, check key routing or repair files. Its exit codes match
`glifistore_inspect_segment`.

### `glifistore_backup_store`

```bash
glifistore_backup_store [--json] [--no-scan] -- /path/to/source /path/to/destination
```

Offline verified copy of a durable data directory; stop writers first. It takes exclusive locks,
verifies the source, creates an empty destination, copies the catalog Segments and then
`manifest.glifi`, syncs, and verifies the destination. Restore is the same command with the backup
as source and a new empty destination ([backup and restore v1](spec/backup-restore-v1.md)).

This offline tool cannot run against a live, locked data directory. For that case use the online
fenced `Store::backup_to` or wire `BACKUP`, which is still not zero-fence hot I/O.

### `glifistore_migrate_store`

```bash
glifistore_migrate_store [--json] [--no-scan] --workers N -- /path/to/source /path/to/destination
```

Offline Worker reshard or logical rewrite; stop writers first. It verifies the source, copies every
live key, value and expiry into a new destination Store created with `--workers N`, checkpoints
progress in `<destination>.migrate-state`, verifies the destination and removes the checkpoint.
After an interruption, run the same command again to resume. The source is never mutated.

The same Worker count is allowed, as an offline rewrite into a new Store identity. Online resharding
is not supported ([store migration](architecture/store-migration.md),
[changing the Worker count](operations/compatibility-and-migration.md#change-the-worker-count)).

### `glifistore_repair_store`

```bash
glifistore_repair_store [--json] [--no-scan] -- /path/to/source /path/to/empty-workspace
```

Offline fail-closed repair that never mutates the source. It requires an explicit empty workspace
and creates:

- `<workspace>/store`: a clean Manifest and catalog Segments, verified after copy;
- `<workspace>/quarantine`: non-catalog namespace anomalies plus `audit.txt`.

Unlisted Segments, crash temporaries, compaction intents and unknown regular files are quarantined.
Missing catalog entries and unsafe entries (symlinks, hard links, non-regular objects) fail closed
without writing a usable store. Live or hot repair is not supported.

### `glifistore_rebuild_index`

Durable v1 persists no separate Index artifact: Indexes rebuild from committed Segments during Store
recovery. This offline rewrite tool therefore permanently refuses Segment-only input and names the
operator path instead:

1. Reopen or restart the Store on the data directory (ordinary recovery).
2. For offline catalog repair, run `glifistore_repair_store` with an explicit empty workspace.

Help and version remain available. Do not use this tool for durable v1 operations.
