# TCP client conformance v1

**Normative** · wire protocol v2 clients (C++, Python, Perl, Go, Erlang, Ruby) · reviewed 2026-09-12

This guide tells client authors **what to implement and how to prove it**. The checklist is
normative for official TCP clients; the pseudocode is descriptive.

Normative behavior is owned by:

- [wire protocol v2](wire-protocol-v2.md) — bytes, opcodes, framing
- [client semantics v1](client-semantics-v1.md) — deadlines, retries, mutation outcomes
- [error taxonomy v1](error-taxonomy-v1.md) — portable categories and fixture vectors

Passing this checklist is required for an “official” SDK claim. It does not raise the GlifiStore
product claim ceiling.

## 1. Session bootstrap (must)

```text
connect(peer)  # TCP host:port or AF_UNIX path (mutually exclusive with TLS)
  with connect_timeout  # not civil clock
INIT → expect OK with worker_count, routing_epoch
for each worker_connection:
  BIND_WORKER(worker_id) → expect OK
  # only then GET/PUT/ERASE/PING/BACKUP/pipeline on that connection
  # HEALTH/READY/STATS are also accepted unbound (wire v2)
```

On reconnect after transport loss: discard the socket; `INIT` + exactly one `BIND_WORKER` before
further Store traffic. Accept only the **original** `worker_count` and `routing_epoch`; mismatch →
`unavailable`, client unhealthy, no automatic mutation retry.

## 2. Request identity and ordering (must)

- Generate unique `request_id` per exchange; never assume server deduplication on id reuse.
- Preserve per-connection pipeline order; do not reorder responses across ids on one socket.
- Route keys with the same Worker hash/routing rules as the server; `WRONG_OWNER` → `protocol`,
  mark unhealthy, open a new client (no online rebalance in v2).

## 3. Error surface (must)

Expose portable `category` from the closed set in error taxonomy v1, plus `message`,
`retryability`, and when known: `wire_status`, `mutation_outcome`, `bytes_sent`, `request_id`,
`worker`, `routing_epoch`, `operation`.

Golden vectors: [`tests/fixtures/error_taxonomy_v1.json`](../../tests/fixtures/error_taxonomy_v1.json).

Malformed / unknown status: keep framing sync; mutations with `bytes_sent > 0` prefer
`indeterminate`.

## 4. Retry and timeout (must)

| Situation | Required behavior |
| --- | --- |
| `GET`/`PING` transport failure | At most one automatic retry after reconnect/bootstrap |
| `PUT`/`ERASE` with `bytes_sent == 0` | At most one automatic retry of the same attempt |
| `PUT`/`ERASE` with `bytes_sent > 0` | No automatic retry; `indeterminate` + reconcile |
| Pipeline / multi-Worker batch | No automatic whole-batch retry |
| Request deadline (monotonic) | Classify `transport`; **reset** connection; do not accept late frames as success |
| `overloaded` / `permission_denied` | `retryability=never` for the same logical mutation without app policy |

Pseudocode (standalone mutation):

```text
send request; track bytes_sent
if transport_fail and bytes_sent == 0 and retries_left:
  reset_connection; bootstrap; retries_left -= 1; goto send
if transport_fail and bytes_sent > 0:
  return indeterminate
decode response; map status → category/outcome per taxonomy
```

## 5. Wire fixture conformance (must)

Decode and/or encode against:

| Fixture | Role |
| --- | --- |
| `tests/fixtures/wire_requests_v2.hex` | Canonical request frames |
| `tests/fixtures/wire_responses_v2.hex` | Canonical response frames |
| `tests/fixtures/error_taxonomy_v1.json` | Status → category/outcome/retryability |

CI interop matrices (cleartext + TLS when toolchains present) exercise PUT→GET, pipeline, binary
values, expiry, structured `NOT_FOUND`, and local oversized rejection. New official SDKs must join
that matrix or document an explicit waiver.

## 6. Optional surfaces (should)

| Feature | Rule |
| --- | --- |
| TLS 1.3 outer transport | ADR 0020 / secure-profile; no new opcodes; refuse with AF_UNIX |
| AF_UNIX dial | ADR 0029; mutually exclusive with TLS |
| Typed `health` / `ready` / `stats` | Wire opcodes 7–9; OK bodies `GlifiStore/live`, `GlifiStore/ready`, bounded `GlifiStore/stats`; do not treat HEALTH as traffic readiness |
| Typed `backup` | Wire `BACKUP` (10); admin under secure authz; fenced semantics |
| Per-call request timeout override | Overrides config for that call only; same reset rules as [client semantics v1 §6](client-semantics-v1.md) |
| Async cancellation | Poison/reset Worker connection; classify via `bytes_sent` |

## 7. Explicit non-goals for clients

- Assuming `request_id` idempotency on the server
- Cancelling admitted durable server work via disconnect
- Online Worker rebalance without a new client ([ADR 0033](../adr/0033-online-rebalance-deferred.md))
- Treating `HEALTH` as traffic readiness (`READY` is required)

## 8. Self-check commands

```bash
# Codec / taxonomy (C++ unit target names may vary by preset)
ctest --test-dir build/<preset> -R 'wire|error_taxonomy|client' --output-on-failure

# Multi-SDK interop (when harness deps installed)
./scripts/test-sdk-interop.sh   # or the CI job documented in CONTRIBUTING
```

## Related

- [Client semantics v1](client-semantics-v1.md)
- [Error taxonomy v1](error-taxonomy-v1.md)
- [SDK roadmap](../architecture/sdk-roadmap.md)
- [ADR 0019](../adr/0019-client-error-retry-timeout.md)
