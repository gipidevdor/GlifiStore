# ADR 0030: Keyed Worker routing (SipHash-2-4 + Manifest seed)

- Status: accepted
- Date: 2026-07-25
- Deciders: security / persistence maintainers
- Applies to: Worker ownership hash; Manifest v1 reserved header field; INIT identity value;
  `glifistored --worker-hash-seed`
- Amends: [ADR 0006](0006-key-routing-hash.md), [ADR 0024](0024-offline-worker-migration.md),
  [ADR 0026](0026-keyed-index-hash-seed.md) (closes the deferred keyed-routing item)
- Supersedes: none
- Depends on: [ADR 0006](0006-key-routing-hash.md), [ADR 0013](0013-native-wire-protocol-v2.md)

## Context

SipHash-2-4 is the keyed PRF of Jean-Philippe Aumasson and Daniel J. Bernstein
(*SipHash: a fast short-input PRF*). GlifiStore ships independent implementations and paper
test vectors; see [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

ADR 0026 keyed the in-memory Index mix seed but left Worker ownership on public FNV-1a. Threat
model §6 still allowed adversarial key crafting that concentrates load on one Worker.

Index seed and Worker routing seed are **separate**: Index state is rebuildable and need not be
persisted; Worker ownership partitions durable Segments and Record `key_hash` metadata, so the
routing seed **must** be durable authority.

## Decision drivers

- Cryptographic Worker placement under `--secure-profile` without breaking default FNV Stores.
- Fail-closed reopen: wrong seed must not silently repartition existing Records.
- Keep protocol v2 frame headers unchanged; disclose seed only when keyed routing is active.
- Prefer server + Manifest correctness; update official clients that parse INIT.

## Alternatives considered

1. **Server-only SipHash while clients keep FNV affinity.** Rejected: perpetual `WRONG_OWNER`.
2. **Share Index seed and Worker seed.** Rejected: different lifecycles.
3. **Protocol v3 header fields.** Deferred; INIT value extension is enough for keyed Stores.
4. **Non-persisted CLI-only seed.** Rejected for durable Stores.

## Decision

1. **Algorithms.** `fnv1a64_v1 = 1` (default) and `siphash24_v1 = 2`. SipHash uses `k0 = seed`,
   `k1 = seed ^ 0x6a09e667f3bcc909`.
2. **Process API.** `set_worker_routing` / `get_worker_routing` / `hash_key_routing`.
3. **Manifest.** Persist `worker_hash_seed` at header offset 84 (8 bytes LE). FNV Stores keep seed
   `0`. Compaction refuses algorithm/seed drift.
4. **Daemon.** `--worker-hash-seed=<u64>` selects SipHash. `--secure-profile` randomizes unless
   pinned. Explicit CLI seed that disagrees with Manifest fails closed on reopen.
5. **Wire.** FNV INIT remains `GlifiStore/2`. Keyed INIT is
   `GlifiStore/2 || 0x00 || u32 algorithm || u64 seed`. Official C++ client parses and routes.
6. **Migration.** Offline Worker-count migration copies source algorithm/seed.

## Consequences

- Positive: Worker floods require knowing the Manifest seed; reopen is fail-closed.
- Negative: keyed daemons need SDKs that understand extended INIT; no online seed rotation.
- Residual: not full multi-tenant isolation (ADR 0028). Official C++ / Python / Perl / Go /
  Erlang / Ruby clients decode plain and extended INIT and route with the disclosed seed.

## Compatibility and migration

- Existing FNV Stores reopen unchanged.
- Algorithm/seed change requires a new Store (or migrate that copies the seed).

## Verification

- Unit tests: routing stability, seed divergence, INIT encode/decode, Manifest round-trip,
  durable reopen match/mismatch, daemon parse/dump.

## References

- [Threat model](../security/threat-model.md) §6
- [Wire protocol v2](../spec/wire-protocol-v2.md) §7
- [Manifest format](../architecture/manifest-format.md)
- [ADR 0006](0006-key-routing-hash.md) · [ADR 0026](0026-keyed-index-hash-seed.md)
