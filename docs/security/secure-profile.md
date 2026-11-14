# Secure profile

**Normative** · `glifistored` TLS, mTLS and authz; SDK TLS credential options · reviewed 2026-07-28

Decisions: [ADR 0020](../adr/0020-tls-outer-transport.md),
[ADR 0021](../adr/0021-secure-profile-authentication.md) and
[ADR 0022](../adr/0022-authorization-capabilities.md).

The secure daemon profile that operators enable before binding beyond a trusted loopback or private
perimeter: what it requires, how it names and authorizes principals, and what it still leaves open.
Protocol v2 framing is unchanged; TLS is an outer transport.

## 1. Profile definition

| Requirement | Mechanism |
| --- | --- |
| Confidentiality and integrity | TLS 1.3 only (`--tls-cert`, `--tls-key`) |
| Authentication | Mutual TLS (`--tls-client-ca`); anonymous peers rejected |
| Authorization | Static principal capability map (`--authz-map`); default deny |
| Worker routing | Keyed SipHash-2-4 advertised in the wire-v2 `INIT` identity; a durable Store persists the seed in its Manifest, a volatile Store keeps it for the process lifetime ([ADR 0030](../adr/0030-keyed-worker-routing.md)) |
| Abuse and DoS bounds | Phase 5 accept, connection and principal rate limits plus idle and request deadlines |
| Fail closed | `--secure-profile` refuses dual cleartext and TLS (`--tls-port`), missing mTLS, a missing authz map, disabled Phase 5 limits, or `--unix-socket` without `--unix-peercred` |

The secure profile is opt-in: trusted cleartext on the default loopback remains valid without it.

**Note.** Official SDKs parse the plain and extended keyed-routing `INIT` identities (ADR 0030). The
secure-profile smoke test, `scripts/test-secure-profile-interop.sh`, covers mTLS, `--authz-map`, the
keyed seed, prefix deny, `--tls-crl`, quotas and keyed routing. The CI job `sdk-clients` runs it
with all six SDKs required (`SECURE_INTEROP_REQUIRE_ALL=1`); local runs skip perl, ruby and erlang
when their TLS toolchains are unavailable. Residual: claiming the complete SDK security train as a
product statement.

### Optional Unix-domain socket (Phase 8, ADR 0029)

`--unix-socket PATH` adds an `AF_UNIX` listener beside TCP and TLS, with the same wire protocol. It
is not a TLS replacement. With `--unix-peercred`, accepted peers map to the authz principal
`unix:uid=<uid>` through Linux `SO_PEERCRED` or BSD and macOS `getpeereid` (OpenBSD: uid and gid
only). Map those principals in `--authz-map` like certificate identities:

```text
unix:uid=1000 write prefix=app/
```

Under `--secure-profile`, `--unix-socket` requires `--unix-peercred` (fail closed). The socket file
mode is owner-only (`0600`). UDS stays cleartext on the host: use it for same-machine sidecars, not
as a substitute for network mTLS.

### TLS 1.3 cipher policy (Phase 2)

Secure-profile listeners and official TLS clients pin these TLS 1.3 AEAD suites only:

- `TLS_AES_128_GCM_SHA256`
- `TLS_AES_256_GCM_SHA384`
- `TLS_CHACHA20_POLY1305_SHA256`

The protocol version policy rejects TLS 1.2 and earlier. SDK connects verify the hostname and CA by
default; lab-only insecure escapes remain explicit.

```bash
glifistored --secure-profile \
  --bind 127.0.0.1 --port 7379 \
  --tls-cert /etc/glifistore/server.crt \
  --tls-key /etc/glifistore/server.key \
  --tls-client-ca /etc/glifistore/clients-ca.crt \
  --authz-map /etc/glifistore/authz.map \
  --tls-crl /etc/glifistore/clients.crl \
  --tls-ocsp-fail-closed \
  --log-format json
```

A non-loopback bind without `--secure-profile` prints a warning. The daemon profile is the intended
boundary for leaving a trusted perimeter; all six official SDKs implement its keyed routing (§6).
Hostile public Internet exposure still needs an explicit CRL (or equivalent revocation) policy,
stronger multi-tenant isolation and physical E3 honesty ([security roadmap](roadmap.md)).

## 1a. Phase 5 abuse controls

| Flag | Secure-profile default | Effect |
| --- | --- | --- |
| `--max-accepts-per-sec` | 128 | Process-wide accept and handshake admissions per second; excess peers are dropped |
| `--idle-timeout-ms` | 60000 | Close quiet connections (monotonic idle) |
| `--request-timeout-ms` | 30000 | Bound partial-frame assembly, in-flight response wait and stalled output progress; admitted Store work is not cancelled |
| `--connection-max-requests-per-sec` | 256 | Per-connection request admission; exceeding it returns wire `overloaded` |
| `--principal-max-requests-per-sec` | 1024 | Per-mTLS-principal request admission |
| `--principal-max-bytes-per-sec` | 32MiB | Per-principal request key and value bytes plus response value bytes |

Trusted cleartext leaves these limits at `0` (disabled). `--secure-profile` replaces zeros with the
defaults above and refuses an explicit `0`.

Only authorized `HEALTH`, `READY` and `STATS` are exempt from the request and bandwidth quotas;
denied attempts, including a denied `STATS`, consume quota before audit. An over-quota denied stream
receives `OVERLOADED` after its earlier decided responses, then closes after a bounded drain.
`STATS` exports the `abuse_*` reject and close counters.

[ADR 0041](../adr/0041-bounded-network-security-lifecycles.md) bounds pending TLS handshakes by
connection slots and an independent deadline (default 10 seconds), without blocking Readers. Pending
output expires after no write progress for `request-timeout-ms`, or `idle-timeout-ms` when the
former is zero. New input does not renew this budget; BIND preserves it. Setting both to zero
explicitly disables output expiry for trusted deployments. Decided replies get a bounded best-effort
flush, and a disconnect never implies that an admitted mutation was cancelled.

A response value that exceeds a principal's remaining byte allowance saturates its window: even a
zero-payload INIT is then refused until the one-second reset. One already-decided response can
exceed the residual allowance; this is not packet-level egress shaping.

## 2. Principal extraction (ADR 0021)

After a successful mTLS handshake, the daemon derives a stable principal id from the verified client
certificate. The first match wins:

1. the first URI SAN (`subjectAltName` uniformResourceIdentifier), such as a SPIFFE ID;
2. otherwise the first DNS SAN;
3. otherwise the subject Common Name (CN).

If none is present, the connection is rejected (fail closed). The principal string is the raw SAN or
CN value, with no type prefix; operators must put that exact string in `--authz-map`.

Credential material never appears in logs, in `--dump-config` (paths only) or in client `Error`
messages.

## 3. Capability map (ADR 0022)

The map is a UTF-8 text file:

```text
# principal capability[,capability...] [prefix=<utf8-bytes>]
reader.example read
writer.example write
admin.example admin
spiffe://cluster.local/ns/app/sa/store-writer write
tenant-a.example write prefix=tenant-a/
tenant-b.example write prefix=tenant-b/
```

| Capability | Allows |
| --- | --- |
| `read` | `GET`, `PING`; `STATS` only when the principal is not prefix-scoped |
| `write` | `PUT`, `ERASE` (implies `read`) |
| `admin` | The reserved admin surface; implies `write` and therefore `read` for v1 convenience; required for `STATS` when the principal has `prefix=` ([ADR 0027](../adr/0027-stats-isolation-prefix-principals.md)) |

- A missing principal is denied the data-plane opcodes with wire `PERMISSION_DENIED` (8).
- With authz enabled, an empty map gives nobody access to the data plane.
- `INIT`, `BIND_WORKER`, `HEALTH` and `READY` need only a verified mTLS principal: any mapped or
  unmapped authenticated peer may bootstrap or probe liveness.
- The optional `prefix=<bytes>` (Phase 8 first slice,
  [ADR 0025](../adr/0025-key-prefix-tenant-scope.md)) requires `GET`, `PUT` and `ERASE` keys to
  start with that exact UTF-8 byte prefix; a mismatch returns `PERMISSION_DENIED`. Omit `prefix=`
  for whole-keyspace access, the ADR 0022 default. An empty `prefix=` is rejected, and prefix values
  must not contain whitespace.
- Prefix-scoped principals cannot read daemon-wide `STATS` without `admin` (ADR 0027). Process
  resources and the durable directory remain shared
  ([ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md), deferred). `--secure-profile`
  randomizes the Index mix seed ([ADR 0026](../adr/0026-keyed-index-hash-seed.md)), and Worker
  ownership uses the keyed SipHash routing state advertised by `INIT`
  ([ADR 0030](../adr/0030-keyed-worker-routing.md)).

## 4. Wire status

`PERMISSION_DENIED` (value 8) is additive within protocol v2. Official clients map it to category
`permission_denied` with `retryability=never`. Mutations are `rejected`.

## 5. Credential provisioning and rotation

1. Operate a private CA (or SPIFFE) that issues short-lived client certificates.
2. Install the server certificate and key and the client CA on the daemon host, with mode `0600`
   for keys.
3. Distribute the client certificate, key and CA to SDKs as filesystem paths; the option names are
   the same across languages: `cert_file`, `key_file`, `ca_file`.
4. To rotate, replace the PEM files on disk, then recycle the daemon (or listener) per deployment
   policy; there is no in-band credential opcode. To revoke, remove the principal from
   `--authz-map`, publish a PEM CRL through `--tls-crl` (fail closed), or both.
   `--tls-ocsp-fail-closed` requires `--tls-crl`; live AIA OCSP HTTP is unsupported. The
   [certificate runbook](../operations/secure-profile-certs.md) has the procedures.
5. Never put PEM bytes in Store options, in environment variables that are logged, or in v2 frames.

## 5a. Security audit trail (Phase 6)

With `--secure-profile` or `--log-format json`, the daemon writes JSON lines on standard error for:

| `event` | `decision` | Fields |
| --- | --- | --- |
| `auth` | `accept` / `deny` | `principal` (accept, or deny when known), `reason` (deny) |
| `authz` | `deny` | `principal`, `opcode`, `reason` |
| `tls` | `error` | `reason` |

The events carry no key or value payloads and no PEM bytes. `--quiet` suppresses only `auth`
accept. `STATS` exports `auth_accepts`, `auth_denies`, `authz_denies` and `tls_errors`, plus
`tls_mtls`, `tls_crl` and `authz_principals`.

Audit decision counters remain exact. One process-wide emitter has a fixed 128-event queue (8 KiB
maximum per event, plus one in flight), and admission and write attempts are each capped at 128 per
monotonic second. Producers use try_lock and perform no sink I/O. A blocked stderr can stall only
that emitter; Readers and Server destruction do not wait for it. Suppression is exported as
`audit_events_suppressed`; `audit_events_written`, `audit_events_pending` and
`audit_events_write_failed` expose delivery progress and failures. Snapshots are concurrent.

The emitter snapshots stderr when the first emitting audit is constructed, leaves O_NONBLOCK and the
process signal dispositions unchanged, and keeps one thread and one descriptor until process exit.
Darwin requires F_SETNOSIGPIPE, and that no-SIGPIPE attribute is shared with stderr and its
duplicates. Nonblocking backpressure waits only in the emitter, which retains the current JSON
line. A hard I/O error disables the sink and suppresses queued and later events until process exit.

Operators must drain the sink for delivery; pending events may be lost at exit. Durable audit, sink
rotation and fork after emitter initialization are unsupported. No hostile-public readiness claim
follows from these bounds.

## 6. SDK checklist

Every official SDK in the same release must expose TLS 1.3 connect options with hostname
verification on by default (`insecure_skip_verify` or an equivalent only in the lab). mTLS uses the
same client certificate and key paths. To claim complete secure-profile compatibility, an SDK must
also parse the plain and extended `INIT` identity, implement keyed SipHash routing, keep the routing
state across reconnects and pass the secure-profile interop matrix. C++, Python, Perl, Go, Erlang
and Ruby meet that routing requirement. Cleartext remains available for trusted deployments.

## 7. Residual risks (still block hostile public bind)

Within the architectural-prototype ceiling, these remain open:

- **The Phase 8 remainder is unsupported** for hostile multi-tenant or public-Internet claims.
  Per-tenant data-directory and process isolation stays deferred
  ([ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md)), and at-rest crypto is unsupported.
  Key-prefix scope, keyed Index and Worker routing and the `STATS` admin gate are a first slice
  only: they improve posture but do not certify adversarial multi-tenant deployments.
  UDS with `SO_PEERCRED` ([ADR 0029](../adr/0029-uds-peercred.md)) is an optional local transport
  and authentication only, not multi-tenant isolation.
- TLS certificate and key material is restart-only: replace the PEMs on disk and recycle the daemon
  (or listener). There is no hot reload and no in-band credential opcode (§5).
- OpenMetrics and Prometheus scrape exporters are intentionally unsupported this cycle
  ([metrics exporters](../operations/observability.md#metrics-exporters)); use wire `STATS` and the
  JSON logs.
- Live AIA OCSP HTTP lookups are intentionally unsupported; use `--tls-crl` and, optionally,
  `--tls-ocsp-fail-closed`.
- A complete SDK security-train claim; the [SDK roadmap](../architecture/sdk-roadmap.md) lists the
  evidence it still needs.
- Physical E3/E4 power-loss certification (storage, not wire).

Phase 5 abuse controls and the Phase 6 audit with local CRL fail-closed are implemented. Configure
`--tls-crl` before calling a deployment public-Internet ready. Production readiness still lists the
multi-tenant Phase 8 remainder and E3 as open.

## References

- [ADR 0020](../adr/0020-tls-outer-transport.md) ·
  [ADR 0021](../adr/0021-secure-profile-authentication.md) ·
  [ADR 0022](../adr/0022-authorization-capabilities.md) ·
  [ADR 0025](../adr/0025-key-prefix-tenant-scope.md) ·
  [ADR 0026](../adr/0026-keyed-index-hash-seed.md) ·
  [ADR 0027](../adr/0027-stats-isolation-prefix-principals.md) ·
  [ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md) ·
  [ADR 0029](../adr/0029-uds-peercred.md) ·
  [ADR 0030](../adr/0030-keyed-worker-routing.md)
- [Threat model](threat-model.md) · [security roadmap](roadmap.md) ·
  [wire protocol v2](../spec/wire-protocol-v2.md) ·
  [client semantics v1](../spec/client-semantics-v1.md)
