# ADR 0025: Key-prefix scope for secure-profile principals (Phase 8 first slice)

- Status: accepted
- Date: 2026-07-25
- Deciders: security maintainers
- Applies to: `glifistored` `--authz-map` / secure-profile admission
- Amends: [ADR 0022](0022-authorization-capabilities.md)
- Supersedes: none
- Depends on: [ADR 0022](0022-authorization-capabilities.md)
- Amended by: [ADR 0027](0027-stats-isolation-prefix-principals.md) (STATS isolation for prefix principals)

## Context

ADR 0022 shipped whole-daemon coarse capabilities (`read` / `write` / `admin`) and deferred
optional **prefix/namespace scope**. Security roadmap Phase 8 lists full multi-tenant adversarial
isolation (keyed routing, hash-flood resistance, stronger quota identity) as later work. Operators
still need a **minimum vertical slice**: mutually distrusting application principals sharing one
daemon must not `GET`/`PUT`/`ERASE` each other's keys when configured.

This ADR freezes that first slice only. It does **not** claim a multi-tenant product.

## Decision drivers

- Fail closed on misconfigured prefix tokens.
- Enforce before Worker admission (same point as capability checks).
- No change to Worker routing hash ([ADR 0006](0006-key-routing-hash.md)).
- Keep the authz map language small and static-file friendly.
- Honest documentation of residual cross-tenant risks (STATS, shared durable directory, hash floods).

## Alternatives considered

1. **One daemon process per tenant (OS isolation only).** Valid deployment pattern; does not help
   shared trusted services that already use one Store.
2. **Per-key ACL tables.** Deferred: hot-path cost and operator burden; still weak without keyed
   routing against adversarial keys.
3. **Separate data directories / Stores per tenant inside one process.** Large runtime redesign;
   deferred with the rest of Phase 8.
4. **Keyed routing version now.** Required for hostile hash-flood resistance (threat model §6);
   deliberately out of this slice.

## Decision

1. Extend `--authz-map` lines to an optional trailing token:

   ```text
   principal capability[,capability...] [prefix=<utf8-bytes>]
   ```

   - Omitting `prefix=` preserves ADR 0022 whole-keyspace behavior for that principal.
   - `prefix=` with an empty value is rejected (fail closed).
   - Prefix values must not contain whitespace (no quoting in v1 of this slice).
   - At most one `prefix=` token per principal line.

2. **Enforcement:** for `GET`, `PUT`, and `ERASE` only, the request key must begin with the
   principal's configured prefix bytes. Mismatch ⇒ wire `PERMISSION_DENIED` (8). Opcodes without a
   Store key (`INIT`, `BIND_WORKER`, `PING`, `HEALTH`, `READY`) ignore prefix for key matching.
   `STATS` ignores prefix matching but, for prefix-scoped principals, requires `admin` instead of
   `read` ([ADR 0027](0027-stats-isolation-prefix-principals.md)).

3. **Secure profile:** prefix scope is optional. `--secure-profile` still requires `--authz-map`
   and default-deny capabilities; it does **not** require every principal to be prefix-scoped.

4. **Not in this slice:** keyed/versioned routing, per-tenant durable directories, row-level
   security, encryption at rest, UDS/`SO_PEERCRED`, or claims that shared-daemon hostile
   multi-tenancy is supported end-to-end.

## Consequences

- Positive: operators can isolate key namespaces for distinct mTLS principals on one daemon.
- Negative: process-level resources and the durable directory remain shared; a compromised
  prefix principal no longer sees daemon-wide `STATS` without `admin` (ADR 0027), but still
  shares the durable directory threat boundary (ADR 0028 deferred).
- Residual: adversarial key sets can still FNV-skew Workers until keyed routing lands (ADR 0026
  defers that); Index mix seed is keyed under secure-profile.

## Compatibility and migration

- Existing authz maps without `prefix=` keep identical semantics.
- Official SDKs need no API change; denial uses existing `PERMISSION_DENIED`.

## Verification

- Unit tests: parse accept/reject; in-prefix allow; cross-prefix deny for GET/PUT/ERASE; lifecycle
  opcodes ignore prefix; unrestricted principals unchanged.
- `--dump-config` reports `authz-prefix-scoped=<count>`.

## References

- [ADR 0022](0022-authorization-capabilities.md)
- [Secure profile](../security/secure-profile.md)
- [Security roadmap](../security/roadmap.md) Phase 8
- [Threat model](../security/threat-model.md)
