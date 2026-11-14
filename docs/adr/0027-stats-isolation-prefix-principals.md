# ADR 0027: STATS isolation for prefix-scoped principals (Phase 8)

- Status: accepted
- Date: 2026-07-25
- Deciders: security maintainers
- Applies to: `glifistored` authz for wire `STATS` (opcode 9)
- Amends: [ADR 0022](0022-authorization-capabilities.md), [ADR 0025](0025-key-prefix-tenant-scope.md)
- Supersedes: none
- Depends on: [ADR 0025](0025-key-prefix-tenant-scope.md)

## Context

ADR 0025 key-prefix scope blocks cross-tenant `GET`/`PUT`/`ERASE` but left `STATS` on the `read`
capability. Daemon-wide counters (connections, abuse, maintenance, authz totals, hot-cache) leak
cross-tenant operational signal. Phase 8 asked to hide or gate STATS for prefix tenants without
pretending full multi-tenant product readiness.

## Decision drivers

- Fail closed under secure-profile + prefix-scoped principals.
- Prefer capability gate over inventing a filtered STATS dialect in this slice.
- Preserve `STATS` for whole-keyspace `read` operators and `admin`.

## Alternatives considered

1. **Filtered per-prefix STATS.** Deferred: no per-tenant counter partition exists yet.
2. **Deny STATS to all non-admin principals.** Too harsh for trusted single-tenant `read` maps.
3. **Leave STATS on `read` for prefix tenants.** Rejected: residual cross-tenant leak called out in
   ADR 0025 consequences.

## Decision

1. When authz is enabled and the principal's grant has a non-empty `prefix=`, `STATS` requires the
   `admin` capability (which implies `write`/`read`).
2. Unscoped principals (no `prefix=`) keep prior behavior: `STATS` needs `read`.
3. Authz disabled (trusted cleartext): unchanged — no capability checks.
4. `HEALTH` / `READY` remain capability-free bootstrap probes (still need mTLS principal under
   secure profile).
5. Denial uses existing wire `PERMISSION_DENIED` (8).

## Consequences

- Positive: prefix tenants cannot scrape daemon-wide STATS with a stolen `read`/`write` cert.
- Negative: prefix tenants that need limited diagnostics must be granted `admin` (full STATS) or
  use out-of-band operator tooling — no tenant-scoped STATS dialect yet.
- Residual: process RSS / OS metrics and shared data-dir remain unisolated (ADR 0028).

## Compatibility and migration

- Authz maps that give prefix-scoped principals only `read`/`write` lose STATS access — intentional.
- Operators who need tenant-readable diagnostics should use an unscoped ops principal or `admin`.

## Verification

- Unit tests: prefix + write denies STATS; prefix + admin allows; unscoped read allows; authz off
  allows.

## References

- [ADR 0022](0022-authorization-capabilities.md) · [ADR 0025](0025-key-prefix-tenant-scope.md)
- [Secure profile](../security/secure-profile.md)
- [Wire protocol v2](../spec/wire-protocol-v2.md)
