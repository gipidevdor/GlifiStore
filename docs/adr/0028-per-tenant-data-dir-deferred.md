# ADR 0028: Per-tenant data directories (deferred)

- Status: proposed
- Date: 2026-07-25
- Deciders: security maintainers
- Applies to: future multi-Store / multi-data-dir daemon shape
- Amends: none
- Supersedes: none
- Depends on: [ADR 0008](0008-alpha-persistence-contract.md), [ADR 0025](0025-key-prefix-tenant-scope.md)

## Context

Phase 8 lists per-tenant durable isolation. Today one `glifistored` process owns one Store and one
data directory. Prefix authz (ADR 0025) only namespaces keys inside that shared durable boundary: a
compromised principal with write still shares segments, compaction, disk quotas, and backup
blast radius with every other principal.

## Decision drivers

- Do not half-break the single-Store model with a partial multi-dir runtime.
- Prefer honest deferral over shipping an unsupported “tenant data-dir” flag.

## Alternatives considered

1. **One daemon process per tenant (OS isolation).** Already a valid *deployment* pattern; does not
   need a new in-process API.
2. **Multiple Stores in one process keyed by prefix.** Large redesign (routing, maintenance,
   STATS, backup, migration); out of scope for this Phase 8 slice.
3. **Bind `prefix=` to a subdirectory under `--data-dir`.** Tempting but would fork durability /
   recovery assumptions without ADR-level format work — rejected for now.

## Decision (proposed / deferred)

No implementation in this release. When revisited, a future accepted ADR must specify:

- Whether tenants are separate Stores vs partitions inside one Store;
- Manifest / segment layout and backup/restore boundaries;
- How Worker routing and maintenance budgets apply;
- Failure isolation and secure-profile defaults.

Until then, operators who need durable isolation must run **separate daemon processes** (separate
`--data-dir`) per trust domain.

## Consequences

- Positive: avoids a false multi-tenant storage claim.
- Negative: shared-daemon prefix tenants still share the durable directory threat boundary.

## Compatibility and migration

- None in this release.

## Verification

- Documentation only until an implementation ADR is accepted.

## References

- [Security roadmap](../security/roadmap.md) Phase 8
- [ADR 0025](0025-key-prefix-tenant-scope.md) · [ADR 0026](0026-keyed-index-hash-seed.md) ·
  [ADR 0027](0027-stats-isolation-prefix-principals.md)
