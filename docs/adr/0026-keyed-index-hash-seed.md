# ADR 0026: Keyed Index mix seed for hash-flood resistance (Phase 8)

- Status: accepted
- Date: 2026-07-25
- Deciders: security maintainers
- Applies to: in-memory Index / SwissTable placement; `glifistored --index-hash-seed`
- Amends: [ADR 0007](0007-swiss-table-index.md) (seed is process-configurable); Index v1 mix constant
- Supersedes: none
- Depends on: [ADR 0004](0004-index-as-derived-state.md), [ADR 0006](0006-key-routing-hash.md), [ADR 0007](0007-swiss-table-index.md)

## Context

Threat model §6 notes that FNV-1a Worker routing and the published Index mix constant are not
hash-flood resistant: an adversary who knows the algorithm can craft keys that concentrate in one
Index bucket chain. Phase 8 asked for keyed / SipHash-class resistance without claiming a full
multi-tenant product.

Worker routing remains FNV-1a on the wire ([ADR 0006](0006-key-routing-hash.md)): official SDKs
compute `fnv1a64(key) % worker_count` and protocol v2 has no seed-disclosure field. Changing
routing requires a versioned algorithm, INIT metadata, and an SDK train — deferred (see below).

Index placement already XORs a 64-bit seed into the mix ([Index v1](../spec/index-v1.md)). That
constant was public. Making the seed secret and process-lifetime is the smallest correct slice.

## Decision drivers

- Improve Index flood resistance under secure-profile multi-principal deployments.
- Do not break protocol v2 client routing or persisted Record `key_hash` (still FNV-1a).
- Keep Index rebuildable without persisting the seed (ADR 0004).
- Honest documentation: Worker skew via FNV remains residual.

## Alternatives considered

1. **SipHash Worker routing with secret seed now.** Rejected for this slice: clients must learn the
   seed; INIT value is an exact ASCII identity today; SDK train required.
2. **Persist seed in the Manifest.** Unnecessary for correctness (Index is derived); adds format
   churn. Operators who need reproducible placement set `--index-hash-seed` explicitly.
3. **Leave the published constant forever.** Rejected for secure-profile hostility posture.

## Decision

1. Expose process-wide `set_index_hash_seed` / `get_index_hash_seed`. Default remains the Index v1
   constant `0x243F6A8885A308D3` so existing tests and trusted cleartext behavior stay bit-stable.
2. Daemon flag `--index-hash-seed=<u64>` sets the seed before `Store::open`.
3. `--secure-profile` **randomizes** the seed from OS entropy unless `--index-hash-seed` is set
   explicitly.
4. SipHash-2-4 is implemented as `siphash24` / `hash_key_keyed` for tests and future keyed routing;
   it is **not** yet used for Worker ownership.
5. **Keyed Worker routing:** implemented in [ADR 0030](0030-keyed-worker-routing.md)
   (`siphash24-v1`, Manifest seed, INIT disclosure). Index seed remains separate and non-persisted.

## Consequences

- Positive: Index bucket targeting requires knowing the process seed.
- Negative: Worker-level FNV skew still possible; seed is not rotated online; changing seed mid-life
  is unsupported.
- Residual: hostile multi-tenant readiness still incomplete (STATS isolation is ADR 0027; data-dir
  isolation deferred in ADR 0028).

## Compatibility and migration

- No wire or Record format change.
- Cleartext / non-secure daemons keep the published default unless configured.
- Secure-profile operators must not assume Index placement is stable across process restarts unless
  they pin `--index-hash-seed`.

## Verification

- Unit tests: seed stability within a process; different seeds diverge placement; SipHash known
  vectors; secure-profile dump-config shows non-default seed when unset.
- Docs: threat model, secure-profile, Index v1, security roadmap Phase 8.

## References

- [Threat model](../security/threat-model.md) §6
- [Security roadmap](../security/roadmap.md) Phase 8
- [Index v1](../spec/index-v1.md)
- [ADR 0006](0006-key-routing-hash.md) · [ADR 0007](0007-swiss-table-index.md)
