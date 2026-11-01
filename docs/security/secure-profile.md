# Secure profile reference

Status: normative for the secure daemon profile (ADR 0020–0022)  
Applies to: `glifistored` TLS/mTLS/authz; official SDK TLS credential options  
Owner: security maintainers  
Last reviewed: 2026-07-28

This document freezes the **secure profile** operators enable before binding beyond a trusted
loopback/private perimeter. Protocol v2 framing is unchanged; TLS is an outer transport
([ADR 0020](../adr/0020-tls-outer-transport.md)).

## 1. Profile definition

| Requirement | Mechanism |
| --- | --- |
| Confidentiality / integrity | TLS 1.3 only (`--tls-cert` / `--tls-key`) |
| Authentication | Mutual TLS (`--tls-client-ca`); anonymous peers rejected |
| Authorization | Static principal capability map (`--authz-map`); default deny |
| Worker routing | Process-lifetime keyed SipHash-2-4 advertised in the wire-v2 `INIT` identity |
| Abuse / DoS bounds | Phase 5 accept/connection/principal rate limits + idle/request deadlines |
| Fail closed | `--secure-profile` refuses dual cleartext+TLS (`--tls-port`), missing mTLS, missing authz map, disabled Phase 5 limits, or `--unix-socket` without `--unix-peercred` |

Trusted cleartext (default loopback) remains valid **without** this profile. The secure profile is
opt-in.

> [!NOTE]
> Official SDKs parse plain and extended keyed-routing `INIT` identities (ADR 0030). Secure-profile
> smoke (`scripts/test-secure-profile-interop.sh`: mTLS + `--authz-map` + keyed seed + prefix deny +
> `--tls-crl`, cpp/python/go + perl/ruby/erlang when their TLS toolchains are available) is in CI.
> Residual: claiming the complete SDK security train as a product statement (smoke covers mTLS,
> authz, prefix, CRL, quotas, and keyed routing; perl/ruby/erlang join when toolchains are present).

### Optional Unix-domain socket (Phase 8 / ADR 0029)

`--unix-socket PATH` adds an `AF_UNIX` listener alongside TCP/TLS (same wire protocol). It is **not**
a TLS replacement. With `--unix-peercred`, accepted peers map to authz principal `unix:uid=<uid>`
via Linux `SO_PEERCRED` or BSD/macOS `getpeereid` (OpenBSD: uid/gid only). Map those principals in
`--authz-map` like cert identities:

```text
unix:uid=1000 write prefix=app/
```

Under `--secure-profile`, `--unix-socket` requires `--unix-peercred` (fail closed). Socket file mode
is owner-only (`0600`). UDS remains cleartext on the host; use it for same-machine sidecars, not as
a substitute for network mTLS.

### TLS 1.3 cipher policy (Phase 2)

Secure-profile listeners and official TLS clients pin TLS 1.3 AEAD suites only:

- `TLS_AES_128_GCM_SHA256`
- `TLS_AES_256_GCM_SHA384`
- `TLS_CHACHA20_POLY1305_SHA256`

TLS 1.2 and earlier are rejected at protocol version policy. Hostname/CA verification is on by
default for SDK connects; lab-only insecure escapes remain explicit.

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

Non-loopback bind without `--secure-profile` prints a warning. The daemon profile is the intended
boundary for leaving a trusted perimeter, but today its SDK compatibility is limited to C++ as
described above. Hostile public Internet still needs an explicit CRL (or equivalent revocation)
policy, stronger multi-tenant isolation, and physical E3 honesty
([security roadmap](roadmap.md)).

## 1a. Phase 5 abuse controls

| Flag | Secure-profile default | Effect |
| --- | --- | --- |
| `--max-accepts-per-sec` | 128 | Process-wide accept/handshake admissions per second; excess peers are dropped |
| `--idle-timeout-ms` | 60000 | Close quiet connections (monotonic idle) |
| `--request-timeout-ms` | 30000 | Bound partial-frame assembly, in-flight response wait and stalled output progress; admitted Store work is not cancelled |
| `--connection-max-requests-per-sec` | 256 | Per-connection request admission; exceed ⇒ wire `overloaded` |
| `--principal-max-requests-per-sec` | 1024 | Per-mTLS-principal request admission |
| `--principal-max-bytes-per-sec` | 32MiB | Per-principal request key+value + response value bytes |

Trusted cleartext leaves these at `0` (disabled). `--secure-profile` fills zeros with the defaults
above and refuses explicit `0`. Only **authorized** `HEALTH` / `READY` / `STATS` stay exempt
from request/bandwidth quotas; denied attempts, including denied STATS, consume them before audit.
An over-quota denied stream receives OVERLOADED after prior decided responses, then closes after
bounded drain. `STATS` exports `abuse_*` reject/close counters.

[ADR 0041](../adr/0041-bounded-network-security-lifecycles.md) bounds pending TLS handshakes by
connection slots and their independent deadline (default 10 seconds), without blocking Readers.
Pending output expires after no write progress for `request-timeout-ms`, falling back to
`idle-timeout-ms` when the former is zero. New input does not renew this budget; BIND preserves it.
Both zero explicitly disables output expiry for trusted deployments. Decided replies receive a
bounded best-effort flush; disconnect never implies that an admitted mutation was cancelled.

Response values exceeding a principal's remaining byte allowance saturate its window; even
zero-payload INIT is subsequently refused until the one-second reset. One already-decided
response can exceed the residual allowance; this is not packet-level egress shaping.

Audit decision counters remain exact. One process-wide emitter has a fixed 128-event queue
(8 KiB maximum per event, plus one in flight); admission and write attempts are each capped at
128 per monotonic second. Producers use try_lock and perform no sink I/O. A blocked stderr can
stall only that emitter; Readers and Server destruction do not wait for it. Suppression is
exported as `audit_events_suppressed`; `audit_events_written`, `audit_events_pending` and
`audit_events_write_failed` expose delivery progress/failures. Snapshots are concurrent.
The emitter snapshots stderr on first emitting audit construction, leaves O_NONBLOCK and process
signal dispositions unchanged, and retains one thread/descriptor until process exit. Darwin
requires F_SETNOSIGPIPE; that no-SIGPIPE attribute is shared with stderr and its duplicates.
Nonblocking backpressure waits only in the emitter while retaining the current JSON line.
A hard I/O error disables the sink, suppressing queued and later events until process exit.
Operators must drain the sink for delivery; pending events may be lost at exit. Durable audit,
sink rotation and fork after emitter initialization are unsupported. No hostile-public readiness
claim follows from these bounds.

## 2. Principal extraction (ADR 0021)

After a successful mTLS handshake, the daemon derives a stable **principal id** from the verified
client certificate. First match wins:

1. First URI SAN (`subjectAltName` uniformResourceIdentifier), e.g. SPIFFE ID  
2. Else first DNS SAN  
3. Else subject Common Name (CN)

If none are present, the connection is rejected (fail closed). The principal string is the raw SAN
or CN value (no type prefix). Operators must put that exact string in `--authz-map`.

Credential material never appears in logs, `--dump-config` (paths only), or client `Error` messages.

## 3. Capability map (ADR 0022)

File format (UTF-8 text):

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
| `read` | `GET`, `PING`; `STATS` only when the principal is **not** prefix-scoped |
| `write` | `PUT`, `ERASE` (implies `read`) |
| `admin` | Reserved admin surface; **implies `write` ⇒ `read`** for v1 convenience; required for `STATS` when the principal has `prefix=` ([ADR 0027](../adr/0027-stats-isolation-prefix-principals.md)) |

Rules:

- Missing principal ⇒ deny data-plane opcodes with wire `PERMISSION_DENIED` (8).  
- Empty map with authz enabled ⇒ nobody can access the data plane.  
- `INIT`, `BIND_WORKER`, `HEALTH`, and `READY` need a verified mTLS principal only (any mapped or
  unmapped authenticated peer may bootstrap / probe liveness).  
- Optional `prefix=<bytes>` (Phase 8 first slice, [ADR 0025](../adr/0025-key-prefix-tenant-scope.md)):
  `GET` / `PUT` / `ERASE` keys must start with that exact UTF-8 byte prefix; mismatch ⇒
  `PERMISSION_DENIED`. Omit `prefix=` for whole-keyspace access (ADR 0022 default). Empty
  `prefix=` is rejected. Prefix values must not contain whitespace.  
- Prefix-scoped principals cannot scrape daemon-wide `STATS` without `admin` (ADR 0027). Process
  resources and the durable directory remain shared ([ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md)
  deferred). Index mix seed is randomized under `--secure-profile`
  ([ADR 0026](../adr/0026-keyed-index-hash-seed.md)); Worker ownership uses the keyed SipHash routing
  state advertised by `INIT` ([ADR 0030](../adr/0030-keyed-worker-routing.md)).

## 4. Wire status

`PERMISSION_DENIED` (value **8**) is additive within protocol v2. Official clients map it to
category `permission_denied` with `retryability=never`. Mutations are `rejected`.

## 5. Credential provisioning and rotation

1. Operate a private CA (or SPIFFE) that issues short-lived client certs.  
2. Install server cert/key and client CA on the daemon host with mode `0600` for keys.  
3. Distribute client cert/key/CA to SDKs via filesystem paths (same option names across languages:
   `cert_file` / `key_file` / `ca_file`).  
4. **Rotation:** replace PEM files on disk, then recycle the daemon (or listener) per deployment
   policy. There is no in-band credential opcode. Revocation: remove the principal from
   `--authz-map` and/or publish a PEM CRL via `--tls-crl` (fail closed). `--tls-ocsp-fail-closed`
   requires `--tls-crl`; live AIA OCSP HTTP is unsupported. See
   [secure-profile-certs.md](../operations/secure-profile-certs.md).  
5. Never put PEM bytes in Store options, env vars that are logged, or v2 frames.

## 5a. Security audit trail (Phase 6)

With `--secure-profile` or `--log-format json`, the daemon emits JSON-lines on stderr for:

| `event` | `decision` | Fields |
| --- | --- | --- |
| `auth` | `accept` / `deny` | `principal` (accept/deny when known), `reason` (deny) |
| `authz` | `deny` | `principal`, `opcode`, `reason` |
| `tls` | `error` | `reason` |

No key/value payloads or PEM bytes. `--quiet` suppresses `auth` accept only. `STATS` exports
`auth_accepts`, `auth_denies`, `authz_denies`, `tls_errors`, plus `tls_mtls` / `tls_crl` /
`authz_principals`.

## 6. SDK checklist

Every official SDK in the same release must expose TLS 1.3 connect options with hostname verify on
by default (lab `insecure_skip_verify` / equivalent only). mTLS uses the same client cert/key paths.
To claim complete secure-profile compatibility it must also parse the plain/extended `INIT`
identity, implement keyed SipHash routing, preserve the routing state across reconnect, and pass the
secure-profile interop matrix. Only C++ currently meets that routing requirement. Cleartext remains
available for trusted deployments.

## 7. Residual risks (still block hostile public bind)

Wave 4 honesty (architectural prototype ceiling):

- **Phase 8 remainder unsupported** for hostile multi-tenant / public-Internet claims:
  per-tenant data-dir / process isolation stays deferred ([ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md));
  at-rest crypto unsupported. Key-prefix + keyed routing + STATS admin gate are first-slice
  only — not adversarial multi-tenant certification.
- **TLS cert/key material is restart-only**: replace PEMs on disk and recycle the daemon
  (or listener). There is no hot reload / in-band credential opcode (§5).
- OpenMetrics/Prometheus scrape exporters are **intentionally unsupported** this cycle
  ([observability.md](../operations/observability.md) §7); use wire STATS + JSON logs.
- Live AIA OCSP HTTP lookups (intentionally unsupported; use `--tls-crl` + optional
  `--tls-ocsp-fail-closed`).
- Hostile multi-tenant isolation beyond key-prefix + keyed Index/Worker routing + STATS gate
  (Phase 8 remainder: per-tenant data-dir / process isolation and at-rest crypto). UDS/`SO_PEERCRED`
  ([ADR 0029](../adr/0029-uds-peercred.md)) is optional local transport/authn only — not multi-tenant
  isolation. Prefix scope + STATS admin gate + keyed Index seed improve posture but do **not**
  certify adversarial multi-tenant deployments.
- Non-C++ SDK support for the keyed-routing identity required by `--secure-profile`.
- Physical E3/E4 power-loss certification (storage, not wire).

Phase 5 abuse controls and Phase 6 audit + local CRL fail-closed are implemented. Configure
`--tls-crl` before calling a deployment public-Internet ready. Production readiness still lists
multi-tenant Phase 8 remainder and E3 as open.

## References

- [ADR 0020](../adr/0020-tls-outer-transport.md) · [ADR 0021](../adr/0021-secure-profile-authentication.md) ·
  [ADR 0022](../adr/0022-authorization-capabilities.md) · [ADR 0025](../adr/0025-key-prefix-tenant-scope.md) ·
  [ADR 0026](../adr/0026-keyed-index-hash-seed.md) · [ADR 0027](../adr/0027-stats-isolation-prefix-principals.md) ·
  [ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md) · [ADR 0029](../adr/0029-uds-peercred.md) ·
  [ADR 0030](../adr/0030-keyed-worker-routing.md)
- [Threat model](threat-model.md) · [Security roadmap](roadmap.md) ·
  [Wire protocol v2](../spec/wire-protocol-v2.md) · [Client semantics v1](../spec/client-semantics-v1.md)
