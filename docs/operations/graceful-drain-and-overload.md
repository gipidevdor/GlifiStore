# Graceful drain and overload

**Descriptive** · `glifistored` wire protocol v2 · reviewed 2026-08-27

Requirement `GS-OPS-SOAK-001`, gate `GATE-OPS-RUNBOOKS`, budget class `OVERLOAD-RUNBOOK` in
[`engineering/performance/budgets.yaml`](../../engineering/performance/budgets.yaml).

How to stop or restart a daemon without abandoning admitted durable work, and how to recognize
overload before it becomes unbounded memory growth or silent data loss.

## Before you start

- Durable deployments use a persistent `--data-dir` (or its config or environment equivalent).
- Load balancers and orchestrators should treat a failing `READY` as not ready for traffic. `HEALTH`
  alone is insufficient during a drain or a sticky storage fault: during shutdown, expect `READY` to
  fail while `HEALTH` may still succeed briefly. The [probe reference](observability.md#probes)
  gives each probe's answer, authorization and failure value.

## Stop or restart

### 1. Stop new traffic

Remove the instance from load balancing, or scale the service to zero, before sending a stop
signal. If the orchestrator supports preStop hooks, confirm that `READY` no longer returns `OK` on
the draining instance.

### 2. Send `SIGTERM` or `SIGINT`

```bash
kill -TERM "$(pidof glifistored)"
# or: systemctl stop glifistored
```

The daemon:

1. stops accepting new TCP connections;
2. drains existing connections: idle connections close after their in-flight responses flush, and
   new requests on draining connections are refused;
3. drains the durable mutations already admitted to Store execution, bounded by
   `--shutdown-drain-ms` (default 30 s; `0` waits without bound);
4. closes the Store and releases the data-directory lock;
5. exits.

A queued mutation that has not entered Store execution when the drain deadline expires completes as
wire `OVERLOADED` (`resource_exhausted`; known not committed). This includes work already dequeued
into a Writer coalescing wait (`min_records` or burst) and work still pending as a later
durable_group sub-batch. In-flight Store work is never cancelled. A timed-out drain makes process
exit fail closed: `join` returns an error and the exit status is non-zero.

The same known-not-committed classification applies when a non-zero mutation queue-wait budget is
the first deadline reached during durable-group coalescing: the Writer closes that wait at the
oldest FIFO deadline instead of extending it to `group-max-wait-ms`.

### 3. Tune the drain deadline if needed

```bash
glifistored --shutdown-drain-ms 120000 --data-dir /var/lib/glifistore ...
# or in daemon.conf:
# shutdown-drain-ms = 120000
# or: GLIFISTORE_SHUTDOWN_DRAIN_MS=120000
```

Raise the deadline when large durable batches or slow storage can exceed 30 s. Use `0` only when
the orchestrator can wait indefinitely and you accept unbounded shutdown time.

### 4. Verify the shutdown

- The process exited: check the service manager or `wait`.
- The data-directory lock is released: offline tools can exclusive-lock the directory.
- If the exit status was non-zero after a timeout, treat in-flight client mutations as
  indeterminate until applications reconcile
  ([client semantics v1 §5](../spec/client-semantics-v1.md)).

### 5. Restart

```bash
glifistored --profile production --data-dir /var/lib/glifistore --bind 0.0.0.0 --port 7379
glifistored --dump-config   # optional: print resolved settings without listening
```

Confirm that `READY` returns `OK` before returning the instance to load balancing.

## Overload

Overload is bounded and fails closed: the server does not grow unbounded queues.

| Condition | Client-visible result | Committed? |
| --- | --- | --- |
| Connection, handoff, input or output buffer limit | Connection closed or `OVERLOADED` | Not committed if rejected before Store entry |
| Durable lane, completion-credit or outstanding-byte limit | `OVERLOADED` | Known not committed |
| Queue wait expires before Store entry | `OVERLOADED` | Known not committed |
| Maintenance emergency or `storage_exhausted` | `OVERLOADED` (the wire cannot distinguish it from admission pressure) | Known not committed |
| Task already inside the Store | Completes or fails normally | Never cancelled mid-commit |

Clients must treat `OVERLOADED` as `retryability=never` for the same logical mutation without
application reconciliation ([client semantics v1](../spec/client-semantics-v1.md),
[ADR 0019](../adr/0019-client-error-retry-timeout.md)). Back off and retry only as a new attempt,
after fixing capacity or shedding load.

### Respond to overload

1. Read `STATS`: the per-shard-pair mutation lane `admitted`, `rejected` and `expired` counters,
   queue depth, maintenance emergency and skip reason, and rotation phase timings.
   `pair_writer_stats` and the wire Worker lane counters name the same topology.
2. Check capacity with `--dump-config`: `--max-store-bytes`, `--reserved-free-bytes`,
   `--max-segments`, and the connection, mutation-lane and buffer limits. `--max-hot-cache-bytes`
   is retained for 0.1.x compatibility, but the paired daemon disables that legacy cache.
3. Shed load: stop new clients, reduce shard-pair hot spots or add instances. A shard-pair (Worker)
   count change requires an offline migration, not an in-place rewrite.
4. Do not kill the process to clear queues. A graceful drain lets admitted work finish within
   `--shutdown-drain-ms`.

## What not to do

- Do not use `SIGKILL` for routine restarts. It skips the drain and may leave client mutations
  indeterminate after commit.
- Do not treat `HEALTH` alone as readiness during deploys or incident traffic cuts.
- Do not assume `OVERLOADED` will succeed on an immediate blind retry. Only operator metrics, not
  the wire status alone, distinguish admission pressure from capacity exhaustion.

## Tests and evidence

Integration coverage includes graceful stop, drain timeout and real-daemon `SIGKILL` paths
(`glifistore_crash_daemon`). The CI and staging runbook smoke, `scripts/exercise_ops_runbooks.sh`
(backup and restore, corruption repair, graceful drain with `STATS`), runs through
`.github/workflows/ops-runbooks.yml`. Gate status is in
[production readiness](../production-readiness.md).

- `scripts/soak_daemon.sh` ends every run with a `SIGTERM` drain, the durability-boundary teardown,
  and never uses a routine `SIGKILL` ([soak profiles](soak.md)).
- The sticky post-commit Writer failure wire test checks that `HEALTH` may still succeed while
  `READY` must fail under degraded durability (`server_reactor_durable_lifecycle_tests` and
  `server_reactor_durable_wire_tests`, case
  `sticky post-commit Writer failure is INTERNAL_ERROR on the wire`).
