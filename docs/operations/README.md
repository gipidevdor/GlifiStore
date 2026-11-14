# Operations

**Descriptive** · durable `glifistored` and the offline maintenance tools · reviewed 2026-08-01

How to start, observe, maintain and recover a durable deployment. These pages describe implemented
behavior. The contracts they rely on are the [CLI reference](../cli.md),
[wire protocol v2](../spec/wire-protocol-v2.md) and
[backup and restore v1](../spec/backup-restore-v1.md). The claim ceiling is architectural prototype.

| Runbook | Use it to |
| --- | --- |
| [Durable daemon](durable-tcp-daemon.md) | Set up a durable `glifistored`: profile, flags, probes, drain, offline tools |
| [Drain and overload](graceful-drain-and-overload.md) | Restart, deploy and handle `OVERLOADED` |
| [Backup and restore](backup-restore.md) | Copy, move or recover a Store |
| [Compatibility and migration](compatibility-and-migration.md) | Upgrade, downgrade or change the Worker count |
| [Corruption and repair](corruption-repair.md) | Handle startup failures, verify errors and namespace anomalies |
| [Observability](observability.md) | Read `HEALTH`, `READY`, `STATS`, logs and the effective configuration |
| [Certificates](secure-profile-certs.md) | Rotate TLS and mTLS material; revoke through the authorization map or CRL |
| [Soak](soak.md) | Run soak profiles from a smoke test to four hours |
| [E3 campaign](e3-campaign.md) | Prepare a pinned-row durability campaign without a false certification |

## First start

1. Choose a profile (`production` or `embedded`) and an absolute `--data-dir`.
2. Run `glifistored … --dump-config`; review abuse limits, maintenance and queue caps.
3. Start the process and confirm the `listen` and `ready` log events (or their JSON form).
4. Probe `READY` before adding the instance to a load balancer. `HEALTH` alone is not enough.

## Rolling restart

1. Remove the instance from the load balancer, or let `READY` fail in a pre-stop hook.
2. Send `SIGTERM` or `SIGINT`; the daemon honors `--shutdown-drain-ms`.
3. Confirm the exit, then start the new binary with the same shard-pair count.
4. Return traffic only after `READY` succeeds.

## Copy and reshape data

| Goal | Path |
| --- | --- |
| Cold verified copy | Offline [backup](backup-restore.md); preferred for release artifacts |
| Live copy | Online `BACKUP` or `Store::backup_to`: a short admission fence, not a zero-fence hot copy |
| Different Worker count | Offline [migration](compatibility-and-migration.md#change-the-worker-count) into a new directory |

Online resharding is not supported ([ADR 0024](../adr/0024-offline-worker-migration.md); constraints
for a future design in [ADR 0033](../adr/0033-online-rebalance-deferred.md)). In-place rewrite of a
data directory is never supported.

## Incidents

**Traffic refused or `OVERLOADED`.** Check `READY` and the `STATS` fields `lane[N].rejected`,
`abuse_*` and queue depths. If `mutations_rejected=1` or maintenance reports an emergency, shed load
and read the maintenance fields. Scale out or lower admission rates; never blindly retry an
overloaded mutation ([client semantics v1](../spec/client-semantics-v1.md)).

**Startup or verify failure.** Stop every writer and run `glifistore_verify_store`. Follow
[corruption and repair](corruption-repair.md); never rewrite a production directory in place.
Restore the last verified backup into a new path and cut over only after it verifies.

**Suspected loss after a crash.** Keep the data directory. Capture E2 or E3 rehearsal artifacts only
with the labels of the [E3 campaign](e3-campaign.md) (`e3_certified=no` until a maintainer promotes
the row). Prefer verify, repair workspace and restore from backup to editing files by hand.

**Certificate or authorization incident.** Rotate material and the CRL as described in
[certificates](secure-profile-certs.md). Prefix principals need `admin` to read `STATS`
([ADR 0027](../adr/0027-stats-isolation-prefix-principals.md)).

## Signals

| Question | Signal |
| --- | --- |
| Is the process alive? | `HEALTH`, log event `listen` |
| May it take traffic? | `READY`, log event `ready` |
| What is it doing? | `STATS` ([observability](observability.md)) |
| What configuration is in effect? | `--dump-config` |

## Maintenance tools

Every maintenance tool follows the [CLI contract](../cli.md): exit `0` on success, `1` on a
validation, I/O or repair failure (fail closed), `2` on a usage error.

Stop every writer that holds the data-directory lock (`glifistored` or an embedded `Store`) before
an offline verify, backup, repair or migration. Online fenced backup (`Store::backup_to`, wire
`BACKUP`) is supported and briefly pauses admissions. Zero-fence hot backup, in-place rewrite and
online resharding are not.

## Exercise

Automated smoke, not multi-hour certification:

```bash
./scripts/exercise_ops_runbooks.sh
./scripts/soak_daemon.sh                 # smoke profile, about 45 s
./scripts/soak_daemon.sh --profile long  # 30 minutes
./scripts/soak_daemon.sh --profile 1h    # one hour, with RSS and STATS samples
```

`.github/workflows/ops-runbooks.yml` runs the runbook exercise on every pull request and push, plus
a weekly 30-minute soak. `.github/workflows/soak-extended.yml` runs the one-hour soak monthly or on
demand. `.github/workflows/durability-evidence.yml` archives E2 collector metadata, a scheduled
process-kill run with a 256-case reproducible randomized crash/reopen campaign, and the linux-ext4
E3 harness smoke. Those uploads are rehearsal artifacts, not E3 or E4 certification; a
maintainer-run pinned campaign follows the [E3 campaign](e3-campaign.md) with
`scripts/run-e3-campaign.sh`.

## Background

- [Server model](../architecture/server-model.md): bounded queues, drain before close, probe
  semantics.
- [Durability and recovery](../architecture/durability-recovery.md): recovery never mutates source
  files.
- [Namespace policy](../architecture/namespace-policy.md): quarantine rules and unsafe entries.
