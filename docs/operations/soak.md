# Soak profiles

**Descriptive** · `scripts/soak_daemon.sh` and the GitHub Actions soak jobs · reviewed 2026-08-27

Requirement `GS-OPS-SOAK-001`, gate `GATE-SOAK`, budget classes `SOAK-SOFTWARE-SMOKE` and
`SOAK-SOFTWARE-EXTENDED` in
[`engineering/performance/budgets.yaml`](../../engineering/performance/budgets.yaml).

How to run the durable `glifistored` soak profiles, from a CI smoke test to four hours, and what
they prove. They are software-path evidence: PUT/GET, reconnect, overwrite churn, graceful drain,
optional RSS and `STATS` sampling, and adversarial software stubs. They are not E3/E4 power-loss
certification, and not a substitute for a controlled multi-hour hardware matrix on release hardware.

## Run

```bash
# Smoke (default)
./scripts/soak_daemon.sh
./scripts/soak_daemon.sh --profile smoke

# Weekly-style local long soak
./scripts/soak_daemon.sh --profile long
# or
SOAK_SECONDS=1800 ./scripts/soak_daemon.sh

# Multi-hour path (RSS + STATS samples; rotation counters may remain 0)
./scripts/soak_daemon.sh --profile 1h
./scripts/soak_daemon.sh --profile 4h

# Adversarial software stubs (default ~45s)
./scripts/soak_daemon.sh --profile hot-key
./scripts/soak_daemon.sh --profile connection-churn
./scripts/soak_daemon.sh --profile queue-saturation
./scripts/soak_daemon.sh --profile adversarial-reclaim
```

The script requires a built `glifistored` and `glifistore_interop_client` (or `GLIFISTORED` and
`GLIFISTORE_INTEROP_CLIENT` pointing at them), plus `lsof` and the Python SDK on `PYTHONPATH` for
`STATS`.

## Profiles

| Profile | Duration | Typical use | RSS and `STATS` sampling |
| --- | --- | --- | --- |
| `smoke` | ~45 s | Every PR and push, through `ops-runbooks.yml` | off |
| `long` | 30 min | Weekly schedule or local | every 60 s |
| `1h` | 1 h | Manual `soak-extended.yml` or monthly schedule | every 60 s |
| `4h` | 4 h | Manual `soak-extended.yml` only | every 120 s |
| `hot-key` | ~45 s | Software stub: hammer one key | off |
| `connection-churn` | ~45 s | Software stub: reconnect every operation | off |
| `queue-saturation` | ~45 s | Software stub: tiny `--durable-mutation-queue-capacity` | off |
| `adversarial-reclaim` | ~45 s | Software stub: heavy overwrite churn | off |

`SOAK_SECONDS` or `--seconds` overrides the duration without changing the profile label. The RSS
fail-closed check still applies when the duration is at least 3600 s or the profile is `1h` or `4h`.

The adversarial profiles (`hot-key`, `connection-churn`, `queue-saturation`, `adversarial-reclaim`)
are software stubs. They exercise shape, `OVERLOADED` under a bounded queue and the `SIGTERM` drain.
They do not close the multi-hour reclaim-fairness residuals (`HAZ-026`, `GATE-CONFIG`) or absolute
hardware budgets.

## What is checked

- Continuous PUT/GET against a durable-periodic daemon (`--open-mode create-new`).
- A reconnect every 25 operations on smoke, long, 1h and 4h, through a fresh TCP client process.
- Overwrite churn every 7 operations, leaving reclaimable sealed history for background vacuum.
- The adversarial stubs above; `queue-saturation` requires at least one `OVERLOADED`.
- Final `STATS` fields, including `useful_compactions=` and `durable_rotations_committed=`.
- A graceful `SIGTERM` drain (`--shutdown-drain-ms`); soak teardown never uses `SIGKILL`.
- Multi-hour only: periodic RSS and `STATS` samples. The run fails if RSS grows by both
  `SOAK_RSS_FAIL_FACTOR` (default 3×) and more than `SOAK_RSS_FAIL_DELTA_KB` (default 256 MiB).

Rotation and useful-compaction counters are reported, not required to be non-zero: short and even
multi-hour software soaks may not fill enough Segments to rotate.

## The `SIGTERM` boundary

Every soak ends with `SIGTERM`, so the daemon takes the graceful drain path: the intended durability
boundary for operator restarts. A `SIGTERM` that races an in-flight mutation still leaves the client
acknowledgement indeterminate after commit, the same rule as a production drain
([graceful drain and overload](graceful-drain-and-overload.md)). Software soaks do not certify
power-loss or `SIGKILL` recovery.

## CI

| Workflow | When | Profile or duration |
| --- | --- | --- |
| `.github/workflows/ops-runbooks.yml` | push or PR | smoke, ~45 s |
| `.github/workflows/ops-runbooks.yml` | weekly, Monday | long, 30 min |
| `.github/workflows/ops-runbooks.yml` | `workflow_dispatch` | caller `soak_seconds` |
| `.github/workflows/soak-extended.yml` | `workflow_dispatch` | `smoke`, `long`, `1h` or `4h` |
| `.github/workflows/soak-extended.yml` | monthly schedule | `1h` |

Multi-hour jobs do not run on every PR. The adversarial stub profiles stay local and optional until
a dedicated CI matrix owns them.

## Related

- [Production readiness](../production-readiness.md): smoke versus multi-hour evidence
- [Operations](README.md) and the [durable TCP daemon](durable-tcp-daemon.md)
