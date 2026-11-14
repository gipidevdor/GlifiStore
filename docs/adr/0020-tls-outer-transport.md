# ADR 0020: TLS as outer transport for the secure daemon profile

- Status: accepted
- Date: 2026-07-20
- Deciders: networking maintainers, security maintainers
- Applies to: `glifistored` network listeners; official SDK connect paths
- Amends: none
- Supersedes: none

## Context

Protocol v2 is cleartext TCP with no authentication
([wire protocol v2](../spec/wire-protocol-v2.md), [threat model](../security/threat-model.md)).
Official posture is loopback / private / sidecar until a secure profile exists
([security roadmap](../security/roadmap.md)). Linux, macOS, FreeBSD, and **OpenBSD** are
architectural targets since `0.1.0`; any TLS design that cannot run on OpenBSD with its system
TLS library is not acceptable as the first-class secure profile.

## Decision drivers

- Confidentiality and integrity of key/value bytes on the wire.
- No silent cleartext downgrade in secure deployments.
- Same SDK release train for C++ / Python / Perl / Go (and Ruby when official).
- Portability: OpenBSD (LibreSSL), macOS, Linux, FreeBSD without a Linux-only TLS stack.
- Preserve protocol v2 framing and client-semantics outcomes (ADR 0013, ADR 0019).
- Operability: operators may terminate TLS at a mesh/proxy, but official clients must still have a
  first-party TLS path to the daemon.

## Alternatives considered

1. **Proxy-only TLS (daemon always cleartext on localhost).** Rejected as the *only* model:
   forces every deployment through an extra hop and leaves official SDKs without a secure connect
   path. Allowed as an *additional* deployment pattern once in-daemon TLS exists.
2. **Opportunistic / STARTTLS upgrade inside v2 frames.** Rejected: complex, downgrade-prone, and
   couples security to protocol versioning.
3. **Custom encryption inside frames.** Rejected: reinvents TLS poorly; breaks middlebox and
   operational norms.
4. **OpenSSL-only (or BoringSSL-only) first release.** Rejected: OpenBSD ships and prefers
   **LibreSSL**; the secure profile must build and be tested against LibreSSL on OpenBSD as a
   release gate, not a best-effort port.
5. **Delay TLS until after Unix-domain sockets.** Rejected: UDS is not a confidentiality control
   for reachable listeners ([server-model.md](../architecture/server-model.md)).

## Decision

1. **Secure profile uses TLS 1.3 as an outer transport.** GlifiStore wire protocol v2 bytes run
   inside the TLS record layer. No new opcode is required for “enable TLS”.
2. **Cleartext and TLS listeners are explicit and separate.** A process may expose one or both only
   via distinct configuration; there is no opportunistic fallback from TLS to cleartext on the same
   logical endpoint.
3. **Minimum TLS 1.3** for the secure profile. Weaker protocols are rejected at config validation.
4. **Certificate and key material** are configured via filesystem paths (and later OS keystores as
   optional); secrets never appear in protocol fields or default logs.
5. **Platform TLS backends (implementation constraint):**
   - OpenBSD: **LibreSSL** (system/`libtls` or LibreSSL `libssl` as chosen in implementation ADR
     notes) — first-class CI target.
   - Other BSDs / Linux / macOS: LibreSSL or OpenSSL 3.x as available; API usage must stay within
     the intersection needed for TLS 1.3 server/client + optional client certs.
6. **Hostname / identity verification** is on by default for SDK secure connects; disabling verify
   is an explicit escape hatch for lab use only.
7. **External TLS termination** remains supported for operators, but does not replace the official
   in-daemon TLS profile or SDK TLS options.

## Consequences

- Positive: protocol v2 and client semantics stay stable; security is a transport concern; OpenBSD
  is not a second-class port.
- Negative: build matrices grow (LibreSSL + OpenSSL); cert lifecycle and rotation become ops work.
- Follow-on: ADR 0021 (authn), ADR 0022 (authz); OpenBSD `pledge`/`unveil` (Phase 6.5) unveils the
  data directory plus configured cert/key/CA/authz paths after `Server::create`, then pledges
  `stdio rpath wpath cpath dpath inet unix flock fattr` (fail closed; no-op on other OS).

## Compatibility and migration

- Existing cleartext loopback deployments remain valid under the **trusted-boundary** posture.
- Secure profile is opt-in until defaults change in a later major/minor documented migration.
- No change to on-disk persistence formats.

## Verification

- Native OpenBSD CI job building daemon + at least one official client against LibreSSL
  (`.github/workflows/openbsd-libressl.yml`, `scripts/ci-openbsd-libressl.sh`).
- Interop: every official SDK PUT→GET over TLS (secure profile).
- Negative tests: TLS 1.2-only peer rejected; missing CA / failed hostname verify fails closed.
- Documented cipher/protocol policy: [secure-profile.md](../security/secure-profile.md).
- TLS tax note: [tls-performance.md](../security/tls-performance.md).

## References

- [Security roadmap](../security/roadmap.md)
- [Threat model](../security/threat-model.md)
- [Wire protocol v2](../spec/wire-protocol-v2.md)
- [ADR 0013](0013-native-wire-protocol-v2.md)
- [Architecture charter](../architecture/architecture-charter.md) (OpenBSD target)

## Amendment — 2026-11-01

[ADR 0041](0041-bounded-network-security-lifecycles.md) amends network-security lifetimes,
output drain, request-attempt admission and audit emission. It preserves persistence v1,
wire v2 and Store acknowledgement points; its explicit disconnect and quota rules govern
stalled or abusive peers.
