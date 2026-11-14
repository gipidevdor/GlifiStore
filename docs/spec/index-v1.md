# SwissTableIndex v1

**Normative** · in-memory Index version 1 · reviewed 2026-08-26

The algorithm of the in-memory Index: how `SwissTableIndex` hashes, probes, inserts, erases,
resizes and compacts, and which invariants verification checks.

## 1. Contract

`SwissTableIndex` maps an arbitrary byte-string key to one `RecordRef`. It is derived, non-persistent state rebuilt from accepted records. It owns copies of keys and never depends on caller memory after insertion.

The Index is not internally synchronized. Its owning Worker must serialize mutation and concurrent access as defined by the concurrency specification.

## 2. Constants and control bytes

| Name | Value | Meaning |
|---|---:|---|
| Group size | 8 slots | probing and control-byte matching unit |
| Empty | `0x80` | slot has never been occupied in the current table |
| Deleted | `0xFE` | tombstone available for reuse; probing must continue |
| Maximum load | 7/8 | resize threshold |
| Inline-key limit | 24 bytes | maximum key stored directly in a slot |

Occupied control bytes are seven-bit fingerprints in `0x01..0x7F`. Zero is remapped to one so no occupied fingerprint can equal a sentinel.

Capacity is a power of two, at least one group, and a multiple of eight.

## 3. Hashes

The Index receives or computes the Store's 64-bit routing hash over every key byte. Ordinary Stores
use FNV-1a 64-bit; keyed Stores use the persisted SipHash-2-4 algorithm and seed from ADR 0030. The
same routing identity must be used when rebuilding or rehashing a Store Index.

Table placement uses a separate mixing step. The default seed is the published constant below;
`--secure-profile` (and `--index-hash-seed`) may replace it with a process-lifetime secret
([ADR 0026](../adr/0026-keyed-index-hash-seed.md)):

```text
x = key_hash XOR seed   # default seed = 0x243F6A8885A308D3
x = (x XOR (x >> 33)) * 0x9E3779B97F4A7C15
x = (x XOR (x >> 29))
```

**Flood-resistance note:** with the published default Index-mix seed, table placement is stable and
reproducible but not secret. A secret Index seed prevents precomputed bucket floods against that
process. Worker routing is a separate domain: FNV-1a remains the compatibility default, while
`--worker-hash-seed` and `--secure-profile` select persisted SipHash-2-4 routing
([ADR 0030](../adr/0030-keyed-worker-routing.md)).

The fingerprint `H2` is the low seven bits of the mixed hash, with zero changed to one. The initial group `H1` is derived from the remaining bits:

```text
group = ((mixed >> 7) AND (group_count - 1)) * 8
```

The low 32 bits of the original key hash are cached in an occupied slot as a fast rejection tag. The tag is not a substitute for full key equality. Rehashing reconstructs the original stable 64-bit hash from the complete owned key bytes.

## 4. Slot and key ownership

A slot contains logically:

- the current `RecordRef`;
- cached 32-bit key-hash tag;
- packed key size and inline/external representation tag;
- a 24-byte union containing either the inline key or an offset in `KeyArena`.

On supported 64-bit targets the physical slot is exactly 64 bytes. The high bit of packed key size is the inline tag, so the implementation rejects keys at or above 2 GiB; Record v1 limits are substantially smaller.

`KeyArena` is a contiguous, bump-allocated byte store for keys longer than 24 bytes. Offsets fit in 32 bits. Individual long keys are not freed on erase; live and dead byte counts trigger arena compaction.

Empty and deleted slots do not represent a key, even if stale payload bytes remain in the slot object.

## 5. Equality

An occupied candidate equals a query only if all conditions hold:

1. control fingerprint matches;
2. key length matches;
3. cached 32-bit key-hash tag matches;
4. all key bytes match.

The empty key is valid. Equality for a zero-length key succeeds after the metadata checks without dereferencing key storage or calling a byte comparison on a null pointer.

## 6. Lookup

Lookup scans groups linearly from `H1`, wrapping at capacity:

1. match the query fingerprint against the group's eight control bytes;
2. test each candidate in slot order using full equality;
3. if equal, return its `RecordRef`;
4. if any control byte in the group is empty, return not found;
5. otherwise continue with the next group.

Deleted controls do not terminate lookup. SIMD may produce the fingerprint and empty masks, but the logical result must equal the scalar eight-byte algorithm on every platform.

## 7. Insert or assign

Before inserting a new key, capacity is ensured such that projected effective occupancy does not
exceed 7/8. Effective occupancy is `live + deleted`, because both states extend a probe chain. The
key hash and mixed hash are computed once per insertion attempt.

Probe groups as for lookup while retaining the first deleted slot encountered:

- if an equal key is found, replace only its `RecordRef` and report assignment;
- on the first empty control, insert into the retained deleted slot if one exists, otherwise into that empty slot;
- write slot payload and owned key bytes before publishing the occupied control byte.

Prepared or batch insertion must reserve all fallible Index and arena capacity before a persistent commit point. Once persistent bytes become authoritative, Index publication must not fail because of an avoidable allocation.

## 8. Erase

Erase locates the exact key. If absent it reports no removal. If present:

- mark the control byte deleted;
- decrement occupied count and increment deleted count;
- for an external key, transfer its length from live-arena bytes to dead-arena bytes;
- destroy or reset nontrivial slot payload as required.

Erase does not backward-shift following entries. The deleted marker preserves their probe chains.

## 9. Resize and rehash

Insertion grows or cleans the table when:

```text
projected_effective_occupancy > capacity - capacity/8
```

If live entries alone require the space, growth doubles capacity. If deleted controls cause the
threshold, the table rebuilds at the same capacity. It may rebuild earlier when deleted controls are
at least `max(8, capacity/4)` and outnumber live entries; this geometric condition avoids repeated
rebuilds during an erase sweep. `erase_no_compact()` only updates counters and controls and never
allocates, preserving durable post-commit publication. A later prepared insertion performs any
required cleanup before persistent I/O.

`reserve(n)` selects a power-of-two capacity large enough that `n` entries remain within the 7/8
limit; equivalently it allows at least `n + ceil(n/7)` slots, normalized to valid group capacity. A
batch reserve also accounts for the worst case in which its inserts encounter empty slots before
unrelated tombstones, so batch publication cannot trigger a post-commit rehash.

Rehash builds a completely independent table and key arena, reconstructs each stable full hash from
the owned key bytes, and reinserts occupied entries. Only after every allocation and insertion
succeeds are the new arrays installed. It eliminates deleted controls and retains only live long-key
bytes. A failed allocation leaves live mappings, controls, counters, capacity, and arena ownership
in the original Index.

## 10. Arena compaction

Arena compaction is considered when dead bytes are both:

- at least 65,536 bytes; and
- at least the number of live bytes.

Compaction copies only live long keys into a new arena and updates their offsets. These thresholds are tuning policy, not persistent format, and may change after benchmark evidence without changing Index semantics.

## 11. Complexity and bounds

Expected lookup, insertion, and erase are constant time under a well-distributed hash. Worst-case time is linear in capacity. Memory is proportional to capacity plus live and not-yet-compacted long-key bytes.

No operation may probe forever: capacity always contains at least one empty slot under the maximum-load invariant.

Statistics expose live size, deleted count, effective load, slot/table/arena bytes, lifetime maximum
probe groups, successful rehashes, and same-capacity tombstone rebuilds. Probe loops remain bounded
to exactly `capacity / 8` groups even if corruption defeats the normal empty-slot invariant.

## 12. Verification invariants

`verify()` or equivalent tests must establish at least:

- control and slot states agree;
- occupied/deleted counters equal observed controls;
- every occupied key is findable at its slot's `RecordRef`;
- all arena spans are in bounds;
- live/dead arena accounting is coherent;
- no duplicate logical keys exist;
- capacity and group alignment are valid;
- probe termination is guaranteed.

Golden tests must cover empty keys, 24/25-byte boundary keys, wraparound, all-deleted groups, replacement, resize, arena compaction, and scalar/SIMD mask equivalence.

## 13. Non-guarantees

Iteration order, slot position, capacity, hash-mixing constants, and memory layout are not public API. The Index is not a persistent file format and must never be serialized by dumping its object representation.
