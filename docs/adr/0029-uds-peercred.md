# ADR 0029: Unix-domain socket transport with optional peer credentials

- Status: accepted
- Date: 2026-07-25
- Deciders: security maintainers, networking maintainers
- Applies to: `glifistored` AF_UNIX listen path; `--authz-map` principal form `unix:uid=N`
- Amends: [ADR 0021](0021-secure-profile-authentication.md) (optional local principal source)
- Supersedes: none
- Depends on: [ADR 0021](0021-secure-profile-authentication.md), [ADR 0022](0022-authorization-capabilities.md)

## Context

Security roadmap Phase 8 lists Unix-domain sockets as a same-host convenience transport with
optional `SO_PEERCRED` / `getpeereid` binding. TCP + mTLS remains the secure-profile transport for
network peers. Local sidecars and operators want a loopback UDS path that can map OS uid into the
existing `--authz-map` without inventing a second capability language.

## Decision drivers

- UDS is **not** a TLS replacement (roadmap principle 5).
- Peer credentials must integrate with `--authz-map` exact-match principals.
- Secure profile must fail closed when UDS is enabled without peercred.
- Linux, macOS, FreeBSD, and OpenBSD are first-class; document gaps honestly.
- Do not change the TCP/TLS accept or mTLS principal path.

## Alternatives considered

1. **UDS without any authn.** Rejected for secure-profile local listeners: anonymous UDS peers would
   bypass mTLS while sharing the Store.
2. **OS uid as sole secure-profile authn (replace mTLS).** Rejected in ADR 0021; still rejected.
3. **Separate peercred→capability map file.** Rejected for v1: `--authz-map` already maps principals.
4. **SPIFFE-style URI for Unix peers.** Deferred; `unix:uid=N` is enough for local maps.

## Decision

1. **`--unix-socket PATH`** binds an additional `AF_UNIX` stream listener (filesystem path). The
   socket is created with owner-only mode (`0600`). Stale path is unlinked on bind and again when
   the owning listener is destroyed. One acceptor (executor 0); `BIND_WORKER` handoff is unchanged.
2. **`--unix-peercred`** extracts peer credentials on accept and sets the connection principal to
   `unix:uid=<decimal>` for `--authz-map` lookup. Platforms:
   - Linux: `SO_PEERCRED` (`struct ucred`: uid, gid, pid)
   - macOS / FreeBSD: `getpeereid` (uid, gid); `LOCAL_PEEREPID` for pid when available
   - OpenBSD: `getpeereid` (uid, gid; pid not reported)
3. **Fail closed:** when `--unix-peercred` is set, accept drops peers whose credentials cannot be
   read and emits an auth deny audit event. `--secure-profile` with `--unix-socket` **requires**
   `--unix-peercred` (and thus `unix_peercred_required`).
4. **Complementary to mTLS:** TCP secure-profile still requires TLS + mTLS + `--authz-map`. UDS
   peercred is an additional local principal source, not a substitute for network mTLS.
5. **OpenBSD:** pledge promise set includes `unix`; the socket parent directory is unveiled `rwc`
   for unlink on shutdown.

## Consequences

- Positive: local clients can be authorized via uid lines in the same authz map as cert principals.
- Negative: uid is forgeable by root/same-uid attackers on the host; filesystem socket ACLs matter.
- Residual: not multi-tenant adversarial isolation; not confidentiality on the host (UDS cleartext).

## Compatibility and migration

- Default unchanged (no UDS listener).
- Authz map lines use the literal principal `unix:uid=1000` (example).
- Wire protocol and TCP/TLS behavior unchanged.

## Verification

- Unit tests: principal formatting; UDS bind/accept + peercred round-trip on Linux/macOS/FreeBSD/OpenBSD.
- Daemon config: `--unix-peercred` requires `--unix-socket`; secure-profile + UDS without peercred fails.
- CI covers Linux; document macOS/FreeBSD/OpenBSD support in secure-profile / roadmap.

## References

- [Security roadmap](../security/roadmap.md) Phase 8
- [Secure profile](../security/secure-profile.md)
- [ADR 0021](0021-secure-profile-authentication.md)
- [ADR 0022](0022-authorization-capabilities.md)
