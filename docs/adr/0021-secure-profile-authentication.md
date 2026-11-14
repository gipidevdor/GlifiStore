# ADR 0021: Authentication for the secure daemon profile

- Status: accepted
- Date: 2026-07-20
- Deciders: security maintainers, networking maintainers
- Applies to: `glifistored` secure profile; official SDK credential options
- Amends: none
- Supersedes: none
- Depends on: [ADR 0020](0020-tls-outer-transport.md)

## Context

Today any TCP peer that can connect may issue Store operations. TLS (ADR 0020) protects the pipe
but does not by itself answer “who is this client?”. Authentication must work on Linux, macOS,
FreeBSD, and **OpenBSD**, including environments that use LibreSSL and may later apply
`pledge`/`unveil` around the daemon.

## Decision drivers

- Fail closed: secure profile rejects anonymous peers.
- Prefer standard TLS mechanisms over inventing a GlifiStore crypto handshake.
- Credential material must not appear in logs, errors, or core-dump–friendly default diagnostics.
- Same principal model across all official SDKs.
- OpenBSD: client certificates and CA files must be compatible with LibreSSL verification.

## Alternatives considered

1. **Cleartext shared secret in the first frame.** Rejected: trivial to sniff without TLS; even with
   TLS, reinventing auth poorly.
2. **mTLS only (no application tokens).** Accepted as the *minimum* v1 secure authn; tokens may be
   added later without removing mTLS.
3. **Token-only over TLS without client certs.** Deferred as sole v1 mechanism: harder to get right
   (issuance, rotation, replay) for a first cut; may follow as ADR amendment.
4. **OS user identity via Unix credentials only.** Rejected as sole model: IPv4 TCP is the primary
   transport; UDS/`getpeereid` is optional local authn per [ADR 0029](0029-uds-peercred.md).

## Decision

1. **v1 secure-profile authentication is mutual TLS (mTLS).** The daemon requires a client
   certificate chain that validates against a configured client CA (or equivalent trust store).
2. **Principal id** for authz (ADR 0022) is derived from verified certificate identity (stable
   mapping documented in the secure-profile reference — typically SAN URI/DNS or subject CN, with
   one canonical choice frozen before SDK API freeze).
3. **Anonymous TLS (server-auth only)** may exist as an explicit lab profile but is **not** the
   secure profile; production secure profile = TLS + client cert required.
4. **Credential lifecycle:** cert/key/CA paths on disk; rotation by replacing files and signaling
   or recycling listeners per runbook; no credentials in Store options or v2 frames.
5. **SDK surface:** connect options for client cert, key, and CA (names aligned across languages);
   failures use existing client error categories (`unavailable` / `invalid_argument` as appropriate)
   without embedding PEM material in `message`.
6. **Future tokens:** a short-lived bearer token *after* TLS remains an allowed extension via a
   later ADR; it must not disable mTLS requirements of the secure profile without an explicit new
   profile name.

## Consequences

- Positive: authn piggybacks on TLS; OpenBSD/LibreSSL path stays unified with ADR 0020.
- Negative: operators must run a minimal CA or cert distribution story.
- Residual: compromised client key ⇒ full principal power until revoked via `--tls-crl` (and/or
  authz-map removal). Live AIA OCSP HTTP is unsupported; `--tls-ocsp-fail-closed` requires CRL.

## Compatibility and migration

- Cleartext trusted deployments unchanged.
- No wire opcode change for mTLS (handshake is TLS-layer).

## Verification

- Daemon rejects connections without client cert when secure profile enabled (all CI OS including
  OpenBSD).
- SDK interop with client certs; redaction tests (no cert/key bytes in errors).
- Documented principal extraction test vectors.

## References

- [ADR 0020](0020-tls-outer-transport.md)
- [ADR 0022](0022-authorization-capabilities.md)
- [Security roadmap](../security/roadmap.md)
- [Threat model](../security/threat-model.md)
