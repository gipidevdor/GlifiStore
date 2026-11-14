# Threat model

**Normative** · embedded engine and TCP protocol v2 · reviewed 2026-07-25

The initial normative security boundary of GlifiStore: what it protects, whom it trusts, which
threats it controls, and which deployments it supports. The ordered implementation plan is the
[security roadmap](roadmap.md).

## 1. Assets

The protected assets are the availability and integrity of Store state, the confidentiality of
key and value data, the correctness of routing and recovery, process memory safety, and exclusive
ownership of a durable directory.

## 2. Trust boundaries

| Boundary | Trust assumption |
| --- | --- |
| C++ caller | Trusted to obey C++ object lifetime; byte contents and sizes are still validated |
| TCP peer | Untrusted |
| Persistent bytes | Malformed or untrusted decoder input, even on a trusted local disk |
| Data-directory parent and OS | Trusted for the configured permissions and the documented filesystem semantics |
| Same-host process | Untrusted unless OS permissions deny namespace or socket access |
| Hardware and filesystem durability | Trusted only to the extent explicitly certified |
| Platform targets | Linux, macOS, FreeBSD and OpenBSD are in scope; OpenBSD uses LibreSSL and applies `pledge`/`unveil` after `Server::create` (Phase 6.5; fail closed) |

## 3. Current security posture

Protocol v2 itself carries no authentication, authorization, tenant isolation or request-quota
identity inside the frame. Optional outer TLS 1.3 is implemented
([ADR 0020](../adr/0020-tls-outer-transport.md)). The secure profile
([ADR 0020](../adr/0020-tls-outer-transport.md)–[0022](../adr/0022-authorization-capabilities.md),
[secure profile](secure-profile.md)) adds TLS 1.3 (Phase 2), mTLS principal extraction (Phase 3),
coarse capabilities (Phase 4), abuse controls (Phase 5), and the Phase 6 authentication audit with
local CRL fail-closed. Cleartext remains the default for trusted loopback.

Anyone who can complete a session on a reachable listener without the secure profile may still
attempt Store operations. The daemon must therefore bind only to an explicitly trusted interface
(default `127.0.0.1`) or run behind a trusted, authenticated proxy or firewall, unless the full
secure profile is enabled (Phases 2–5).

Hostile public Internet exposure still wants an operator-configured CRL and physical E3 honesty;
live OCSP HTTP remains unsupported. Full multi-tenant isolation remains Phase 8: key-prefix scope is
a first slice only ([ADR 0025](../adr/0025-key-prefix-tenant-scope.md)).

CRC32C detects accidental corruption. It is not a MAC and gives no protection against deliberate
byte modification. File permissions and directory ownership are the current confidentiality and
integrity boundary for persistence.

## 3a. Attacker profiles (planning)

| Profile | Attacker capability | Intended control (target) |
| --- | --- | --- |
| Local OS user | Can open loopback ports or read world-readable sockets | OS permissions; default bind `127.0.0.1`; OpenBSD `unveil` of the data directory and the certificate and authz paths after create |
| On-path LAN | Sees cleartext TCP on a non-loopback bind | Do not bind non-loopback without the secure profile; TLS (ADR 0020) |
| Compromised authenticated client | Valid client certificate for one principal | Capability limits (ADR 0022); revoke and rotate certificates; Phase 5 quotas |
| TLS sidecar or mesh peer | Terminates TLS before the daemon | The daemon may stay cleartext only on a strictly private interface; the official secure profile still prefers in-daemon TLS |
| Malicious stored bytes | Crafted Segments or Manifests | Existing fail-closed codecs and fuzzing; TLS does not solve this |
| Hostile multi-tenant share | Many principals, adversarial keys | Unsupported as a product; prefix scope, the `STATS` gate, the keyed Index seed ([ADR 0025](../adr/0025-key-prefix-tenant-scope.md)–[0027](../adr/0027-stats-isolation-prefix-principals.md)) and keyed Worker routing ([ADR 0030](../adr/0030-keyed-worker-routing.md)) improve posture; data-directory isolation is still open |

## 4. Primary threats and controls

| Threat | Existing control | Residual risk or required work |
| --- | --- | --- |
| Oversized or malformed frame | Checked lengths, 2 MiB frame maximum, bounded buffers, close on unsafe framing | CPU-rate DoS mitigated by the Phase 5 admission and rate limits when enabled |
| Slow reader or writer | Bounded connection buffers and backpressure; `--idle-timeout-ms` and `--request-timeout-ms` | Tune deadlines for the workload; Store mutations in execution are never cancelled |
| Connection-handoff flood | Bounded MPSC queue; close on saturation; `--max-accepts-per-sec` | Fair admission counters in `STATS` |
| Crafted persistent length or version | Checked arithmetic, exact codecs, allocation bounds, fail-closed recovery | Continuous fuzzing required |
| Symlink or special-file namespace attack | Descriptor-relative audit and strict engine namespace | Platform-specific audit and privilege guidance required; OpenBSD `unveil` confines the daemon to the data directory and the configured TLS and authz paths |
| Concurrent directory open | Exclusive lock file | Network filesystems may not satisfy the required semantics |
| Torn or reordered write | Checksummed alternating commit slots and sync ordering | Power-loss certification is platform dependent (OpenBSD `fdatasync`≡`fsync` cost) |
| Malicious disk modification | Checksums and identities detect many changes | No cryptographic authenticity or encryption |
| Memory exhaustion | Configured resource preflight, bounded queues and caches | Allocator fragmentation and many connections require limits |
| Cross-tenant key access | Optional `--authz-map` `prefix=` ([ADR 0025](../adr/0025-key-prefix-tenant-scope.md)); prefix `STATS` needs `admin` ([ADR 0027](../adr/0027-stats-isolation-prefix-principals.md)); keyed Index mix seed ([ADR 0026](../adr/0026-keyed-index-hash-seed.md)); keyed Worker routing ([ADR 0030](../adr/0030-keyed-worker-routing.md)) | Shared data directory (ADR 0028 deferred); hostile multi-tenancy still unsupported |
| Cleartext eavesdropping | Trusted bind and network boundary; optional TLS 1.3 outer transport when enabled | Enable TLS (ADR 0020) before non-loopback exposure; LibreSSL on OpenBSD |
| Anonymous remote mutation | mTLS with `--authz-map` or `--secure-profile` (default deny; wire `PERMISSION_DENIED`) | Configure `--tls-crl` before hostile public Internet exposure; multi-tenant Phase 8 |
| Anonymous local UDS mutation | Optional `--unix-peercred` → `unix:uid=N` in `--authz-map` ([ADR 0029](../adr/0029-uds-peercred.md)); the secure profile requires peercred when UDS is enabled; socket mode `0600` | UDS is cleartext on the host; root or the same uid can forge the uid; not a TLS replacement |

## 5. Fail-closed requirement

When persistent authority is ambiguous, or runtime publication can no longer agree with durable
state, the Store stops serving the affected operations and reports an error. It must not guess a
Manifest, adopt an orphan, ignore committed corruption or continue after an unrecoverable
post-commit publication failure.

Failing closed protects integrity but can reduce availability. Repair and salvage must be a
separate, explicit operator mode that never silently changes normal-open semantics.

Secure-profile misconfiguration (a missing CA, a failed client verification, an empty capability
map) must likewise fail closed rather than degrade to anonymous cleartext.

## 6. Availability limits

Deployments must configure limits for connections, input and output buffering, handoff capacity,
Store resources, descriptors and reserved disk space. Phase 5 adds optional accept, connection and
principal rate limits and idle and request deadlines; `--secure-profile` applies non-zero
defaults. Exposing the daemon to hostile networks without the secure profile remains unsupported.

Hash-flood resistance is partial. Worker routing defaults to public FNV-1a and may use keyed
SipHash-2-4 with a Manifest-persisted seed ([ADR 0030](../adr/0030-keyed-worker-routing.md));
`--secure-profile` randomizes that seed unless `--worker-hash-seed` pins it. Index placement uses a
separate process-lifetime keyed mix seed ([ADR 0026](../adr/0026-keyed-index-hash-seed.md)).
Hostile multi-tenant readiness remains incomplete without per-tenant data-directory isolation
([ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md)).

## 7. Secrets and logging

Keys and values may be sensitive. Errors and logs should report sizes, identifiers, offsets and
stable categories without dumping payload bytes. Core dumps, benchmark fixtures, quarantined files
and crash artifacts can contain data and need operator-controlled permissions and retention.

No credentials should be stored in Store options or protocol fields until a credential lifecycle
and redaction policy exists. TLS private keys and client certificates must never be written to logs
or to client `Error` messages.

Backup and restore of a secure-profile host must copy the certificate, key, CA and CRL files and the
`--authz-map` paths with the same permissions as the data directory (mode `0600` for keys); see the
[certificate runbook](../operations/secure-profile-certs.md).

## 8. Security change requirements

A network exposure change must define authentication, authorization granularity, TLS termination,
replay and retry semantics, rate limiting, timeout behavior and audit logging. A
persistent-authenticity change must define key management, rotation, backup and restore, and the
interaction with compaction and migration.

Security-sensitive parsers require fuzz targets and bounds tests. Platform namespace changes require
review of symlinks, hard links, rename races, permissions and filesystem semantics. An OpenBSD
change that adds `pledge` or `unveil` requires an explicit review of the promise set, so that TLS,
data-directory I/O and logging keep working without ambient home-directory access.

## 9. Supported deployment statement

The current safe deployment model is a single trusted application using the embedded API, or a
daemon reachable only by mutually trusted local or private clients and protected by operating-system
and network controls. Public Internet exposure and hostile multi-tenancy are not supported.

Optional key-prefix scope in `--authz-map` can separate Store key namespaces for distinct mTLS
principals; it does not make shared-daemon adversarial multi-tenancy a supported product.

Decisions: [ADR 0020](../adr/0020-tls-outer-transport.md),
[ADR 0021](../adr/0021-secure-profile-authentication.md),
[ADR 0022](../adr/0022-authorization-capabilities.md),
[ADR 0025](../adr/0025-key-prefix-tenant-scope.md),
[ADR 0026](../adr/0026-keyed-index-hash-seed.md),
[ADR 0027](../adr/0027-stats-isolation-prefix-principals.md),
[ADR 0028](../adr/0028-per-tenant-data-dir-deferred.md),
[ADR 0029](../adr/0029-uds-peercred.md),
[ADR 0030](../adr/0030-keyed-worker-routing.md).
