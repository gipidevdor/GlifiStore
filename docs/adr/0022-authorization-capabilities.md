# ADR 0022: Authorization capabilities for the secure daemon profile

- Status: accepted
- Date: 2026-07-20
- Deciders: security maintainers
- Applies to: `glifistored` secure profile admission control
- Amends: none
- Supersedes: none
- Depends on: [ADR 0021](0021-secure-profile-authentication.md)
- Amended by: [ADR 0025](0025-key-prefix-tenant-scope.md) (optional key-prefix scope)

## Context

After mTLS establishes a principal (ADR 0021), the daemon still needs a default-deny rule for
*what* that principal may do. Multi-tenant hostile isolation (keyed routing, per-tenant quotas) is
explicitly out of scope for the first authz cut ([threat model](../security/threat-model.md) §6).
Authorization must remain simple enough to implement and test on OpenBSD alongside Linux/macOS/FreeBSD.

## Decision drivers

- Default deny in the secure profile.
- Coarse capabilities first; avoid a full ACL language in v1.
- Enforcement before mutation admission and before returning sensitive read payloads.
- No change to Worker routing hash (ADR 0006) in this phase.

## Alternatives considered

1. **Authn only (all authenticated peers are full admin).** Rejected for secure profile: one stolen
   laptop cert would own the Store with no operator knob.
2. **Per-key ACL list.** Deferred: large design, hot-path cost, and weak without tenant isolation.
3. **Namespace-as-process (one daemon per tenant).** Remains a valid *deployment* pattern; does not
   replace in-daemon capabilities for shared trusted services.
4. **POSIX-style UID mapping from certs.** Rejected as primary model on a network daemon.

## Decision

1. **v1 capabilities** (exact names frozen in the secure-profile reference before SDK freeze):

   | Capability | Allows |
   | --- | --- |
   | `read` | `GET`, `PING` (and read-only admin diagnostics if exposed) |
   | `write` | `PUT`, `ERASE` (implies `read` unless explicitly split by config — default: `write` includes `read`) |
   | `admin` | process-level controls reserved for future admin surface; not required for data plane v1 |

2. **Binding:** each principal maps to a capability set via daemon config (static file first;
   dynamic directory later). Missing map ⇒ **deny**.
3. **Enforcement points:** after TLS+mTLS success, before request admission to Worker queues;
   denied requests return a stable protocol status (existing or documented addition — prefer an
   existing status if semantics fit; otherwise a versioned wire amendment via new ADR). Prefer
   **no new wire version** if `INVALID_REQUEST` / dedicated status can be specified without
   ambiguity in client semantics.
4. **Scope:** whole-daemon capabilities for v1. Optional **prefix/namespace scope** is specified by
   [ADR 0025](0025-key-prefix-tenant-scope.md) (Phase 8 first slice); it is not required to ship mTLS.
5. **Secure profile default:** require authn + explicit capability map; empty map means nobody can
   access data plane.
6. **Not in v1 authz:** adversarial multi-tenant hash-flood resistance, per-key ACL, row-level
   security, or encryption-at-rest.

## Consequences

- Positive: small policy surface; easy to reason about on OpenBSD deployments with few service
  principals.
- Negative: shared daemon with mutually distrusting tenants remains unsupported.
- Follow-on: identity-aware quotas (security roadmap Phase 5.3).

## Compatibility and migration

- Trusted cleartext profile: no capability checks (OS/network boundary remains the control).
- Secure profile: deny by default.

## Verification

- Matrix tests: `read`-only principal cannot `PUT`; `write` can `PUT`/`GET`; unmapped principal
  denied.
- CI on OpenBSD included in the secure-profile job set.
- Client semantics updated if a new wire status is introduced.

## References

- [ADR 0020](0020-tls-outer-transport.md)
- [ADR 0021](0021-secure-profile-authentication.md)
- [Security roadmap](../security/roadmap.md)
- [Threat model](../security/threat-model.md)
- [ADR 0006](0006-key-routing-hash.md)

## Amendment — 2026-11-01

[ADR 0041](0041-bounded-network-security-lifecycles.md) amends network-security lifetimes,
output drain, request-attempt admission and audit emission. It preserves persistence v1,
wire v2 and Store acknowledgement points; its explicit disconnect and quota rules govern
stalled or abusive peers.
