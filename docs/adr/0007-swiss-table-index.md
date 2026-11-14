# ADR 0007: SwissTable Index partitions

- Status: accepted
- Date: 2026-07-11
- Amended by: [0026](0026-keyed-index-hash-seed.md) (process-configurable Index mix seed)

Each Worker owns an Index partition implemented as a flat open-addressing SwissTable-style table with
8-slot groups, control-byte fingerprints (H2), H1 group probing, seeded key hashing, inline key
storage up to 24 bytes, and heap spill for longer keys. The bootstrap `std::unordered_map` backend
is removed from the runtime; recovery scratch maps during segment scan remain offline-only.

This is an **independent reimplementation** of SwissTable-style layout ideas popularized by Abseil
Swiss Tables (Apache-2.0). No Abseil source is included; Abseil’s license does not attach to
GlifiStore source. See [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

The table is the production shape. SIMD group matching, reference-based keys, and incremental
resize tuning are performance refinements on this layout, not alternate architectures.

The supported 64-bit layout fixes each slot at 64 bytes. Inline bytes and the long-key arena offset
share storage; key length and representation mode are packed; a 32-bit hash tag rejects almost all
fingerprint collisions before the complete byte comparison. The tag is never a correctness
authority, and resize reconstructs the stable 64-bit hash from owned key bytes. This trades extra
rehash CPU for a permanent 20% reduction from the former 80-byte slot and better cache density on
the steady-state lookup path.
