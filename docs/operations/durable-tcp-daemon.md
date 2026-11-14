# Durable TCP daemon

**Descriptive** · durable `glifistored`, wire v2, paired Reader–Writer runtime · reviewed 2026-07-31

How to configure, probe, stop and maintain a durable `glifistored`. The normative flag and wire
rules are in the [CLI reference](../cli.md) and [wire protocol v2](../spec/wire-protocol-v2.md);
installed builds also carry `man 8 glifistored` and `man 7 glifistore`
([mdoc pages](../../man/README.md)).

In 0.1.0 the daemon runs only paired shard pairs, each one Reader (Reactor) and one serial Writer.
Prefer `--shard-pairs`; `--workers` is a 0.1.x alias.

## Quick reference

```bash
# Start (production durable)
glifistored --profile production --data-dir /var/lib/glifistore --bind 0.0.0.0 --port 7379

# Validate config
glifistored --profile production --data-dir /var/lib/glifistore --dump-config

# Graceful stop (orchestrator preStop)
kill -TERM "$(pidof glifistored)"

# Offline verify + backup (daemon stopped)
glifistore_verify_store -- /var/lib/glifistore
glifistore_backup_store -- /var/lib/glifistore /backup/empty-dest
```

## Profile and storage mode

A deployment profile is the recommended starting point. Its preset values sit between the
hardcoded defaults and the file, environment and CLI overrides; an unknown profile name fails
closed before listen.

| Profile | Storage | Maintenance | Use |
| --- | --- | --- | --- |
| `dev` | `volatile` | disabled | Local iteration only; not for durable data |
| `embedded` | `durable-periodic` | background | Sidecar or single node, with tighter caps |
| `production` | `durable-periodic` | background | Standard durable deployment |

Every durable profile still requires an explicit data directory; a profile never invents a path.

```bash
glifistored --profile production --data-dir /var/lib/glifistore \
  --bind 0.0.0.0 --port 7379
```

The equivalent config file:

```ini
profile = production
data-dir = /var/lib/glifistore
bind = 0.0.0.0
port = 7379
```

Settings resolve as defaults < profile < config file < environment < CLI. Check the resolved
settings without listening:

```bash
glifistored --profile production --data-dir /var/lib/glifistore --dump-config
```

When a profile does not match your durability contract, set `--storage-mode` explicitly. Every
durable mode requires `--data-dir` (or `data-dir`, or `GLIFISTORE_DATA_DIR`).

| Mode | Acknowledgement contract (summary) |
| --- | --- |
| `durable-sync` | Each mutation is durable before wire success |
| `durable-periodic` | Batched flush on interval and size limits; the profile default |
| `durable-group` | Strict group commit with bounded concurrent producers |

```bash
glifistored --storage-mode durable-sync --data-dir /var/lib/glifistore ...
```

Durable-only flags (`--open-mode`, batch limits, resource caps) are rejected with volatile storage.

`--open-mode` sets the open policy:

| Mode | Behavior |
| --- | --- |
| `open-or-create` | Default: create an empty Store or open an existing one |
| `create-new` | Fail if the directory already contains a Store |
| `open-existing` | Fail if no Store exists; for restore or migration cutover |

Invalid combinations, and a durable mode without `--data-dir`, fail before the process binds a
socket.

## Resource, batch and maintenance flags

Tune these after `--dump-config` on representative hardware. The defaults are safe starting points;
the `embedded` profile lowers several caps automatically.

| Flag | Default (`production`) | `embedded` preset | Purpose |
| --- | --- | --- | --- |
| `--max-store-bytes` | 8 GiB | 1 GiB | Cap catalog Segment bytes |
| `--reserved-free-bytes` | 256 MiB | 64 MiB | Keep filesystem headroom unused |
| `--max-segments` | 127 | 32 | Cap the Segment count |
| `--max-hot-cache-bytes` | 256 MiB | 64 MiB | Accepted 0.1.x compatibility limit; the paired daemon disables the legacy hot cache |
| `--max-temporary-compaction-bytes` | 1 GiB | 256 MiB | Cap the compaction temporary peak |

### Batching

For `durable-periodic`, and for the periodic batch fields shared with group limits:

| Flag | Default (periodic / group mode) | Purpose |
| --- | --- | --- |
| `--sync-interval-ms` | 1000 | Periodic flush interval |
| `--group-max-records` | 4096 / 32 | Batch record limit |
| `--group-max-bytes` | 4 MiB / 64 KiB | Batch byte limit |
| `--group-max-wait-ms` | 1000 / 10 | Batch close wait |

In 0.1.0 every shard has exactly one Writer. The former `--durable-group-concurrency` setting is
removed: group aggregation is a Writer-owned policy, never a way to create competing shard mutators.

The Writer drains up to `group-max-records` from its bounded SPSC lane. It waits up to the
configured deadline to satisfy `min-records`, then uses only a short, bounded burst-coalescing
window. All staged records share the commit boundary and are published before their completions
become visible to the Reader (Reactor). When `--durable-mutation-queue-wait-ms` is non-zero, the
oldest admitted mutation's inclusive pre-Store deadline is an earlier batch-close boundary: reaching
it produces `OVERLOADED` without entering the Store, and the later group deadline never extends the
queueing.

### Admission

| Flag | Default | Purpose |
| --- | --- | --- |
| `--durable-mutation-queue-capacity` | 256 | Admitted mutations per shard pair |
| `--durable-mutation-queue-bytes` | 16 MiB | Owned mutation bytes per shard pair |
| `--durable-mutation-queue-wait-ms` | 0 | Optional pre-Store expiry; `0` disables it |

When these limits or a maintenance emergency are hit, clients see `OVERLOADED`. Treat it as not
committed without application reconciliation
([client semantics v1 §5](../spec/client-semantics-v1.md)). The
[drain and overload runbook](graceful-drain-and-overload.md) lists every overload outcome.

### Maintenance

| Flag | Default | Purpose |
| --- | --- | --- |
| `--maintenance-mode` | `background` | `cooperative`, `background` or `disabled` |
| `--maintenance-max-copy-bytes-per-cycle` | 128 MiB | Normal compaction copy limit (`0` = unlimited) |
| `--maintenance-max-copy-bytes-per-sec` | 0 generic; 64 MiB/s `embedded`; 128 MiB/s `production` | Pace bounded private pre-intent compaction writes (`0` = unlimited; the profile values are overrideable starting points) |
| `--maintenance-max-cpu-ms-per-window` | 0 | Normal one-second compaction CPU budget (`0` = unlimited) |
| `--maintenance-suspend-on-p99-latency-ms` | 0 | Defer normal compaction when the latest durable mutation p99 reaches this threshold (`0` = disabled; pressure and emergency bypass it) |
| `--maintenance-suspend-on-p99-min-samples` | 32 | Minimum completed mutations for a latency decision |
| `--maintenance-max-latency-deferral-ms` | 30000 | Admit one normal reclaim attempt after continuous latency deferral (`0` = wait until pressure) |
| `--maintenance-unread-ttl-pressure-probe` | true | Probe unread expired puts under pressure |
| `--maintenance-unread-ttl-normal-scheduling` | false | Opt in: count unread expired bytes in the normal dead-byte threshold |

Pressure and emergency maintenance bypass the normal copy and rate limits to recover capacity. A
sticky maintenance fault fails `READY` until it is resolved or the Store is replaced from backup.

The rate limiter never caps a whole transaction: a candidate larger than one second of bandwidth
continues in bounded physical writes. Tune it on the deployment filesystem with foreground p99 and
p99.9, compaction completion time and the pacing `STATS` fields. There is no universal cross-device
optimum.

## Probes and deadlines

Wire opcodes 7–9 (`HEALTH`, `READY`, `STATS`) are accepted before `INIT`/`BIND_WORKER` and never
mutate Store state. Orchestrators must gate traffic on `READY`, not on `HEALTH` alone: `HEALTH`
reports process and executor liveness (`started && !failed_`), not paired Writer sticky health.
During a graceful drain or a sticky storage fault, `HEALTH` may succeed while `READY` fails (wire
status `internal_error`). The daemon main loop keys exit on `live()`, not `healthy()`, so a sticky
fail-closed state does not make the process exit while `HEALTH` still answers. The
[probe table](observability.md#probes) gives each answer and condition.

Under `--authz-map` or `--secure-profile`, `STATS` requires the `read` capability for an unscoped
principal; a prefix-scoped principal needs `admin`
([ADR 0027](../adr/0027-stats-isolation-prefix-principals.md),
[secure profile](../security/secure-profile.md)). On a cleartext trusted bind `STATS` remains an
unauthenticated admin surface, so restrict network access to it. With the Phase 5 limits enabled,
it exports the `abuse_*` reject and close counters.

`--idle-timeout-ms` and `--request-timeout-ms` bound quiet connections and stalled partial or
in-flight requests. The secure profile applies non-zero defaults; the cleartext default is `0`
(off). These daemon-side deadlines are orthogonal to client monotonic request deadlines
([client semantics v1 §6](../spec/client-semantics-v1.md)).

| Event | Daemon action | Admitted durable Store mutation | Cold read |
| --- | --- | --- | --- |
| Idle quiet connection | Close TCP; `abuse_idle_closed` | n/a (no in-flight request) | n/a |
| Partial frame or in-flight request exceeds `--request-timeout-ms` | Close TCP; `abuse_request_timeout_closed` | Not cancelled: it completes and the client must reconcile (`indeterminate` if bytes were sent) | Cancelled through the connection read-cancellation epoch |

Wire v2 has no cancel frame. Closing the socket is the server's only signal, and it must not roll
back linearized mutations.

With `--log-format json`, the daemon writes the `ready`, `maintenance_emergency`,
`maintenance_fault` and shutdown drain events to standard error for log aggregation.

## Shutdown drain

`SIGINT` and `SIGTERM` stop new TCP connections, drain existing connections (idle ones close once
their in-flight responses flush) and the durable mutations already admitted to Store execution,
close the Store, release the data-directory lock and exit. `--shutdown-drain-ms` bounds the drain:
30 s by default; `0` waits without bound.

A queued mutation that never enters Store execution before the deadline completes as wire
`OVERLOADED` (known not committed). In-flight Store work is never cancelled. A timed-out drain makes
process exit fail closed (non-zero exit). The
[drain runbook](graceful-drain-and-overload.md) covers the details.

```bash
glifistored --shutdown-drain-ms 120000 --data-dir /var/lib/glifistore ...
# GLIFISTORE_SHUTDOWN_DRAIN_MS=120000
```

To restart:

1. Remove the instance from load balancing.
2. Confirm that `READY` fails.
3. Send `SIGTERM`.
4. Verify a clean exit and the release of the data-directory lock.
5. Restart the daemon.
6. Confirm `READY` before returning traffic.

Do not use `SIGKILL` for routine restarts.

## Offline backup, verify and repair

Every maintenance tool requires stopped writers: the daemon must not hold the data-directory lock.

| Task | Tool | Reference |
| --- | --- | --- |
| Structural verify | `glifistore_verify_store` | [CLI reference](../cli.md#glifistore_verify_store), [corruption and repair](corruption-repair.md) |
| Offline backup or restore | `glifistore_backup_store` | [Backup and restore](backup-restore.md) |
| Salvage with quarantine | `glifistore_repair_store` | [Corruption and repair](corruption-repair.md) |
| Segment forensics | `glifistore_inspect_segment` | [Corruption and repair](corruption-repair.md) |

Typical planned maintenance:

```bash
systemctl stop glifistored
glifistore_verify_store -- /var/lib/glifistore
install -d -m 700 /backup/glifistore-$(date +%F)
glifistore_backup_store -- /var/lib/glifistore /backup/glifistore-$(date +%F)
systemctl start glifistored
```

A restore always targets a new empty directory. Cut over at the orchestration layer (symlink, mount
or configuration), never by overwriting the live path in place.

## Shortcuts and limits

| Claim or shortcut | Status |
| --- | --- |
| Offline `glifistore_backup_store` while `glifistored` holds the lock | Not supported: the offline tool fails closed on the exclusive Store lock |
| Online fenced backup (`BACKUP`, `Server::backup_to`) | Supported: an admission pause for flush, structural check and catalog copy; destination verify after resume; not zero-fence hot I/O |
| In-place restore over production data | Forbidden: copy into a new path, verify, then swap |
| Filesystem snapshot without stopped writers | Insufficient: freeze writers and run `glifistore_verify_store` on the image |
| `glifistore_rebuild_index` for durable v1 | Permanently refused: Indexes rebuild through Store recovery or `glifistore_repair_store` |
| E3/E4 sudden power-loss certification | Pending: process-kill (E2), the E3 harness and operator campaign preparation (`scripts/run-e3-campaign.sh`) exist; no pinned native filesystem row is certified ([platform durability evidence](../architecture/platform-durability-evidence.md), [E3 campaign](e3-campaign.md)) |
| `HEALTH` as readiness during deploys | Wrong signal: use `READY` |
| Blind retry after `OVERLOADED` | Unsafe: the same logical mutation may be uncommitted; reconcile or start a new attempt |

NFS, SMB, FUSE, overlay and remote or user-space storage are outside the local-filesystem contract
([persistence v1](../spec/persistence-v1.md)).

## Related

- [CLI reference](../cli.md): every flag, configuration precedence and the maintenance tools
- [Operations](README.md): the drain, backup and corruption runbooks
- [Server model](../architecture/server-model.md): bounded queues, Reactor and durable lanes
- [Production readiness](../production-readiness.md): release gates and evidence levels
- [v1 production roadmap](../v1-production-roadmap.md): P0-01, the daemon durability backlog
