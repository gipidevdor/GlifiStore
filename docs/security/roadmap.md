# Security roadmap

**Roadmap** · daemon TCP surface, official SDKs, durable namespace operations · reviewed 2026-08-26

The ordered plan that turns the [threat model](threat-model.md) and the security rows of
[production readiness](../production-readiness.md) and the
[persistence v1 production roadmap](../v1-production-roadmap.md) into implemented controls. It does
not redefine wire or persistence contracts; those changes require ADRs and compatibility evidence
first.

Phases 1 to 6 are done. The open work is the log and error redaction audit (item 0.3), the
supply-chain track (Phase 7) and the items that Phase 8 leaves unsupported or deferred.

## Non-negotiable principles

1. Correctness before exposure. Fail-closed storage and recovery rules are never weakened for
   authentication convenience.
2. Same train for all official SDKs. TLS or session-authentication changes ship for C++, Python,
   Perl, Go, Erlang and Ruby in the same release. Cleartext remains an explicit development default,
   but no official SDK may silently lack requested TLS or mTLS support or fall back to cleartext
   ([SDK roadmap](../architecture/sdk-roadmap.md)).
3. Secure profiles fail closed. A deployment that asks for TLS or authentication must not fall back
   to cleartext or anonymous access by default.
4. The OS and network boundary is not a product feature. Loopback, private or sidecar deployment
   remains the documented posture even though Phases 2–4 are implemented; hostile public and
   adversarial multi-tenant deployments remain outside the supported claim.
5. UDS is not security. The Unix-domain transport and its optional peer credentials are
   complementary same-host controls; they do not replace TLS and authentication on reachable
   listeners ([server model](../architecture/server-model.md)).
6. No payload secrets in logs. Logs carry categories, sizes and request IDs, never key or value
   dumps ([threat model](threat-model.md) §7).
7. OpenBSD is first-class. Secure-profile TLS uses LibreSSL on OpenBSD, and CI and docs must not
   treat OpenBSD as a best-effort port ([ADR 0020](../adr/0020-tls-outer-transport.md)). OpenBSD is
   required, not optional, even when native builders and LibreSSL API quirks stretch the TLS work.
   The implemented `pledge`/`unveil` hardening is not a substitute for TLS.

## Current baseline

| Area | Today |
| --- | --- |
| Transport | Cleartext TCP (protocol v2) by default; optional TLS 1.3 outer transport when built with LibreSSL or OpenSSL (`--tls-cert`/`--tls-key`); optional dual cleartext and TLS listeners through `--tls-port` (ADR 0020) |
| Authentication and authorization | Secure profile: mTLS principals and coarse `--authz-map` capabilities (ADR 0021, 0022); the cleartext trusted profile is unchanged |
| Quotas and rate limits | Phase 5 abuse controls: accept, connection and principal rate limits; idle and request deadlines (secure-profile defaults; `0`, off, in cleartext) |
| Audit | Phase 6: `auth`, `authz` and `tls` JSON audit events and `STATS` counters; lifecycle JSON logs |
| At-rest cryptography | None: permissions and CRC32C; CRC is not a MAC |
| Revocation | Optional fail-closed `--tls-crl`; `--tls-ocsp-fail-closed` requires a CRL; no live OCSP over HTTP |
| Safe deployment | Trusted loopback or private deployment; official SDKs decode the keyed-routing `INIT` extension (ADR 0030) |

The daemon controls through Phase 6 are implemented, but exposure to the Internet or to hostile
multi-tenant use remains unsupported. Hostile public Internet exposure still needs the multi-tenant
work of Phase 8 and physical E3 evidence. Live OCSP over HTTP is unsupported by design; CRL is the
fail-closed revocation path.

## Platform matrix

| OS | Expected TLS library | Extra confinement |
| --- | --- | --- |
| OpenBSD | LibreSSL (system) | `pledge`/`unveil` for the data directory and certificate paths (done, Phase 6.5) |
| FreeBSD | LibreSSL or OpenSSL 3.x | Capsicum evaluation later (an existing v1 roadmap note) |
| Linux | OpenSSL 3.x or LibreSSL | Standard file permissions; optional Landlock later |
| macOS | LibreSSL or OpenSSL as provided by the build | Keychain integration optional later |

## Phase dependencies

```text
Phase 0  Posture & hygiene
    │
Phase 1  Decisions (threat model + ADRs)
    ├──────────────┬────────────────┐
Phase 2  TLS       Phase 5a DoS     Phase 7 Supply chain
    │              (can overlap)     (can overlap)
Phase 3  Authn
    │
Phase 4  Authz
    │
Phase 5b Quotas / idle timeouts (identity-aware)
    │
Phase 6  Audit & admin surface
    │
Phase 8  Later: at-rest crypto, per-tenant isolation (UDS and keyed routing done)
```

## Phase 0 — Posture and hygiene

Goal: document the unsafe surface and close the documentation gaps that block security design. New
wire bytes are out of scope.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 0.1 | Daemon, README and SDK connect docs state cleartext and no authentication | Done: daemon banner and SDK READMEs |
| 0.2 | Bind defaults stay loopback-oriented; a non-loopback bind is a documented explicit opt-in | Done: CLI help and stderr warning |
| 0.3 | Log and error redaction audit: no key or value bytes in default logs | Ongoing; policy in [threat model](threat-model.md) §7 |
| 0.4 | Link this roadmap from the docs index and production readiness | Done |

## Phase 1 — Security architecture decisions (done 2026-07-20)

A gate: freeze what to build before coding TLS and authentication into the wire.

| ID | Deliverable | Status |
| --- | --- | --- |
| 1.1 | Threat model expanded with attacker profiles | Done: [threat model](threat-model.md) §3a |
| 1.2 | Transport-security ADR | Done: [ADR 0020](../adr/0020-tls-outer-transport.md), outer TLS 1.3; LibreSSL on OpenBSD |
| 1.3 | Authentication ADR | Done: [ADR 0021](../adr/0021-secure-profile-authentication.md), mTLS v1 |
| 1.4 | Authorization ADR | Done: [ADR 0022](../adr/0022-authorization-capabilities.md), coarse capabilities |
| 1.5 | Specification sketch: TLS wrapper, protocol v2 unchanged | Done in ADR 0020 |
| 1.6 | Security release process stub | Done: [SECURITY.md](../../SECURITY.md), reporting and supported window |

## Phase 2 — Transport security with TLS (done 2026-07-23)

Goal: confidentiality and integrity of the byte stream, with optional client identity through
certificates.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 2.1 | Daemon secure listen profile: certificate, key, CA, minimum TLS 1.3, cipher policy | Done: `TlsContext` with `--tls-cert`/`--tls-key`/`--tls-client-ca`; TLS 1.3 only; explicit AEAD cipher suites; CMake `GLIFISTORE_ENABLE_TLS` (LibreSSL on OpenBSD, OpenSSL 3.x elsewhere); [secure profile](secure-profile.md) |
| 2.2 | Cleartext and TLS listeners through explicit flags; no dual-mode opportunistic TLS | Done 2026-07-20: TLS without `--tls-port` keeps `--port` TLS-only; `--tls-port` enables dual cleartext and TLS on distinct ports and fails closed if the ports collide; never opportunistic on one endpoint |
| 2.3 | Official SDK TLS connect options: CA, certificate, hostname verification on by default in the secure profile | Done for C++, Python, Perl, Go, Erlang and Ruby: opt-in TLS 1.3 (`tls`/`Enable`, `ca_file`/`tls_ca`, `cert_file`/`key_file`, `server_name`, insecure lab escape); fail closed; no silent cleartext fallback |
| 2.4 | Interop matrix: every SDK PUT→GET over TLS | Done: `test-sdk-interop.sh` cleartext and TLS matrices with ephemeral certificates; Erlang included when OTP is available; Perl TLS soft-excluded when `IO::Socket::SSL` is missing |
| 2.5 | TLS tax measured on the same harness as the Go TCP benchmarks | Done 2026-07-20: `scripts/benchmark_tls_tax.sh` and [TLS performance](tls-performance.md); the Go bench gained `--tls` flags |
| 2.6 | OpenBSD CI: native LibreSSL build and TLS smoke | Done 2026-07-20: `.github/workflows/openbsd-libressl.yml` through `vmactions/openbsd-vm` and `scripts/ci-openbsd-libressl.sh` (LibreSSL-only configure, the complete registered suite partitioned across two deterministic shards, Go TLS PUT→GET) |

The build option `GLIFISTORE_ENABLE_TLS=AUTO`, the default, enables TLS when LibreSSL or OpenSSL is
found; `ON` fails configure when neither is found; `OFF` forces a cleartext-only build. OpenBSD
prefers the system LibreSSL; macOS and Linux typically need OpenSSL 3.x (set `OPENSSL_ROOT_DIR` when
it is not on the default path). Daemon TLS invocations are in the
[command-line interface](../cli.md#tls).

On the wire, TLS is the outer transport and protocol v2 is unchanged (no opcode churn): item 1.5
settled this in ADR 0020, which rejects an in-band upgrade.

## Phase 3 — Authentication (done 2026-07-23: mTLS principals)

Goal: every admitted session has a stable principal id, or is rejected. mTLS and local principals
ship first; federation comes later.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 3.1 | Principal model: certificate CN or SAN, token subject, or both, per the ADR | Done: URI SAN → DNS SAN → CN ([secure profile](secure-profile.md) §2) |
| 3.2 | Session establishment after TLS, if token-based: one authentication exchange, then normal v2 operations | Not applicable to mTLS-only v1: the handshake is the authentication exchange |
| 3.3 | Credential provisioning docs: files, environment, rotation without downtime | Done: [secure profile](secure-profile.md) §5 |
| 3.4 | SDK credential hooks with no secrets in `Error.Error()`, logs or `inspect` | Done: existing TLS options; only paths in dumps and logs |
| 3.5 | Reject anonymous peers when the secure profile is enabled | Done: `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` and principal extraction |

## Phase 4 — Authorization (done 2026-07-23: coarse capabilities)

Goal: principals are limited to the operations and namespaces they are allowed.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 4.1 | Capability table, such as `read`, `write`, `admin` (exact set in the ADR) | Done: [secure profile](secure-profile.md) §3 |
| 4.2 | Enforcement points: before mutation admission; explicit GET and PING policy | Done: Reactor pre-admission; wire `PERMISSION_DENIED` |
| 4.3 | Namespace or prefix scope, if the ADR requires it | Done 2026-07-25 as the first Phase 8 slice: [ADR 0025](../adr/0025-key-prefix-tenant-scope.md); keyed Worker routing is done separately under [ADR 0030](../adr/0030-keyed-worker-routing.md) |
| 4.4 | Default deny in the secure profile | Done: `--secure-profile`; an empty map or an unmapped principal is denied |

## Phase 5 — Abuse controls and limits (done 2026-07-25)

Goal: bound the damage from authenticated or pre-authentication peers (DoS, slowloris, fan-out).
The phase is split so that transport-agnostic work can start early.

| ID | When | Deliverable | Acceptance |
| --- | --- | --- | --- |
| 5.1 | Parallel to Phase 2 | Connection and handshake rate limits; maximum connections | Done: `--max-accepts-per-sec` and the existing `--max-connections`; `STATS` `abuse_accepts_rejected` |
| 5.2 | Parallel to Phase 2 | Daemon-side idle and request deadlines, aligned with client semantics | Done: `--idle-timeout-ms` and `--request-timeout-ms` (monotonic); Store mutations already executing are never cancelled |
| 5.3 | After Phase 3 | Per-principal request and bandwidth quotas | Done: `--principal-max-requests-per-sec`, `--principal-max-bytes-per-sec` and `--connection-max-requests-per-sec` |
| 5.4 | After Phase 3 | Pipeline and frame admission policy under overload | Done: existing frame and buffer bounds; an exceeded quota returns wire `overloaded`; an accept flood is dropped |

`--secure-profile` applies non-zero Phase 5 defaults and refuses an explicit `0` (fail closed).
Trusted cleartext keeps the limits at `0` (disabled) unless the operator sets them.

## Phase 6 — Audit, administration and operability (done 2026-07-25; local CRL, live OCSP deferred)

Goal: operators can see who did what, and manage secure deployments from documented procedures.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 6.1 | Security audit events: connect, authentication success or failure, authorization deny, TLS errors | Done: JSON-lines `auth`/`authz`/`tls` events through `SecurityAudit`, without payloads; enabled by `--secure-profile` or `--log-format json` |
| 6.2 | Administrative and diagnostic surface: listener mode, TLS status, principal counts, no secret material | Done: `STATS` `tls_*`, `authz_*`, `auth_*`, `tls_errors`; readiness sticky rules unchanged |
| 6.3 | Runbooks: rotate certificates or tokens, revoke a principal, respond to an incident | Done: [certificate rotation and revocation](../operations/secure-profile-certs.md) |
| 6.4 | Backup and restore note on credentials and data-directory permissions | Done: [threat model](threat-model.md) §7 and the certificate runbook |
| 6.5 | OpenBSD `pledge`/`unveil` after `Server::create`, covering the data directory and the TLS and authz paths | Done 2026-07-25: applied fail closed; the CRL path is unveiled; no-op on Linux, macOS and FreeBSD |

`--tls-crl` enables fail-closed CRL checking for mTLS. `--tls-ocsp-fail-closed` requires
`--tls-crl`: live AIA OCSP over HTTP is unsupported under OpenBSD `pledge` without `dns`.

Item 6.5 is platform confinement, not a substitute for revocation configuration on hostile binds.

## Phase 7 — Supply chain and security process (parallel track)

Goal: trust the shipped bits, not only the wire. Effort: ongoing; bootstrap 1–2 weeks.

| ID | Deliverable | Acceptance |
| --- | --- | --- |
| 7.1 | Dependency and secret scanning in CI | Partial: gitleaks and Trivy fs; pull-request dependency review; OpenSSF Scorecard; actionlint and the SHA-pin validator, including the Dependabot `github-actions` SHA and floating-tag discipline check; CodeQL for C/C++, Python, Go and Actions, with SARIF upload opt-in through `GLIFISTORE_UPLOAD_SARIF` |
| 7.2 | SBOM, checksums and signatures for release artifacts | Partial: checksums and SPDX; checksum-pinned Syft install; Cosign keyless bundles on tags; GitHub SLSA attestations; C++ install-prefix SBOM on tags; GitHub Release attach (`.github/workflows/release.yml`); project GPG optional. **Retained-evidence gap:** no tagged Cosign, SBOM or attestation pointers under `engineering/evidence/release/` yet |
| 7.2a | Copyright and third-party notices | Done: `NOTICE`, `THIRD_PARTY_NOTICES.md`, `docs/legal/licensing.md`, `REUSE.toml`, install and CI license and notice checks |
| 7.3 | Fuzz regression intake for parsers (wire and persistence) | Documented owners |
| 7.4 | Supported security maintenance window | Partial: [SECURITY.md](../../SECURITY.md) publishes the prototype window (`main` and `0.1.x` get best-effort fixes; earlier snapshots are unsupported); formal beta, RC and stable support windows remain open (`GATE-VERSION-LIFECYCLE` residual risk) |
| 7.5 | Branch protection and required checks | Settings checklist: [GitHub branch protection](../assurance/github-branch-protection.md) |

## Phase 8 — Later and optional work

Phase 8 follows the secure TCP profile. Do not schedule these items as blockers for leaving
loopback. The remaining items **stay unsupported or deferred** for architectural-prototype and
hostile-public claims until a dedicated ADR, proofs and evidence close them.

| Item | Status |
| --- | --- |
| Key-prefix capability scope (shared daemon) | Done 2026-07-25: [ADR 0025](../adr/0025-key-prefix-tenant-scope.md); optional `prefix=` in `--authz-map` |
| Keyed Index mix seed (hash-flood slice) | Done 2026-07-25: [ADR 0026](../adr/0026-keyed-index-hash-seed.md); `--index-hash-seed`; the secure profile randomizes it |
| `STATS` isolation for prefix principals | Done 2026-07-25: [ADR 0027](../adr/0027-stats-isolation-prefix-principals.md); a prefix principal needs `admin` for `STATS` |
| Per-tenant data-directory or Store isolation | Unsupported, deferred: [ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md) (proposed); use one process per trust domain |
| Unix-domain socket transport | Done 2026-07-25: [ADR 0029](../adr/0029-uds-peercred.md); `--unix-socket` and optional `--unix-peercred` (`unix:uid=N`); not a TLS replacement |
| At-rest encryption or MAC for Segments | Unsupported, deferred: key management, rotation, compaction and backup make it a large design |
| Keyed Worker routing (SipHash and wire seed) | Done 2026-07-31: [ADR 0030](../adr/0030-keyed-worker-routing.md); daemon, plus `INIT` decode and SipHash routing in the C++, Python, Perl, Go, Erlang and Ruby SDKs; default Stores stay FNV |
| Protocol-level compression or multiplexing | Unsupported, deferred: unrelated; separate ADRs |
| OpenMetrics or Prometheus exporter | Unsupported (intentional): [observability](../operations/observability.md#metrics-exporters); wire `STATS` and JSON logs only |
| Hot TLS certificate reload | Unsupported: restart-only PEM replacement ([secure profile](secure-profile.md) §5 and §7) |

## SDK train checklist

Every security release checks:

- the specification or ADR is merged before the SDK API freeze;
- the C++ client;
- Python, sync and async;
- Perl;
- Go;
- Erlang;
- Ruby;
- the interop harness is green on the secure profile;
- the cleartext profile is still tested for trusted deployments (explicit, never the default in
  secure docs);
- the changelog and migration notes explain how to enable TLS or authentication without downtime.

## Definition of done for the security beta

- Accepted ADRs for TLS, authentication and authorization granularity.
- A secure profile with TLS, authentication, default-deny authorization and basic rate and idle
  limits.
- All official SDKs in the same release.
- An updated threat model, with every residual-risk row closed or explicitly deferred to Phase 8.
- Audit events and a rotate-and-revoke runbook.
- Supply-chain scanning and a named vulnerability-response owner.
- Documented unsupported surfaces: public Internet multi-tenancy, at-rest cryptography and others.

## Related documents

| Document | Role |
| --- | --- |
| [Threat model](threat-model.md) | Assets, boundaries and residual risks: the authority on threats |
| [Persistence v1 production roadmap](../v1-production-roadmap.md) | The broader P1 security and storage-namespace items |
| [SDK roadmap](../architecture/sdk-roadmap.md) | The SDK train constraint for TLS and authentication |
| [Production readiness](../production-readiness.md) | Checklist gates for beta and release candidate |
