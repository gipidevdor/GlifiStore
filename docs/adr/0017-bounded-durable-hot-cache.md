# ADR 0017: Bounded durable hot cache and active-Segment fallback

- Status: accepted
- Date: 2026-07-19
- Owners: storage and performance maintainers
- Related: ADR 0004, ADR 0008, ADR 0009, ADR 0011, ADR 0016

## Context

Durable PUT previously duplicated every active-Segment key and value in an unbounded per-Worker
`unordered_map`. Entries survived until rotation, so memory scaled with recent write volume and a
large-value GET copied the value while holding the Worker mutex. Removing the cache outright would
move every durable GET to file I/O and regress the dominant hot path.

The immutable generation reader is opened when the Segment is published. Its ordinary committed
boundary therefore does not include Records appended later to the active generation. A cache miss
cannot safely read such a Record unless the runtime has an exact generation pin and revalidates the
authoritative Index after I/O.

## Decision drivers

- hard, observable cache admission limits without a global cache lock;
- no value-sized copy or file I/O under the Worker mutex;
- no `RecordRef`, descriptor, or Segment lifetime outside the mutex without generation ownership;
- no fallible allocation after a persistent commit;
- preserve hot-read throughput while making cache exhaustion a correctness-neutral state.

## Decision

The global hot-cache byte budget is deterministically partitioned across Workers and capped by an
explicit per-Worker byte limit. Each Worker also has entry and pre-commit staging-byte limits.
Admission is also gated by `hot_cache_enabled` and `max_hot_cache_value_bytes` (default 64 KiB):
oversized values never admit and cannot blow the Worker budget. The per-Worker table is a Swiss-style
flat open-addressed map (H2 control fingerprints, 8-slot SIMD/scalar group probe, maximum load 0.75,
geometric growth, values inlined up to 48 bytes). Its bucket arrays are allocated lazily on first
successful admission; disabled and never-used Worker caches allocate no table. The slot derives the
inline/heap representation from `value_size`, takes sequence identity only from its authoritative
`RecordRef`, and keeps resident byte charge outside the slot while publication is staged. Accounting
charges the complete bucket arrays once and external resident key/value payload separately, avoiding
a second per-entry charge for the same fixed slot. Strict-group staging additionally charges one temporary slot, its pending
mutation, and duplicated publication key. The counters, hit/miss/stale/eviction/size-rejected
counts, hit-rate, and effective limits are available as a single locked Worker snapshot; no global
cache mutex or shared admission counter is introduced.

PUT prepares table capacity and a single-allocation `shared_ptr<const byte[]>` value (when above the
inline threshold) before the first persistent write. Publication only moves prepared ownership. If
any cache limit is exhausted, admission is bypassed; after authoritative Index publication an older
cached value is removed and the mutation still succeeds. Erase, replacement, failed group
publication, and rotation release their exact charges. A zero limit or `hot_cache_enabled=false`
intentionally disables admission.

A hot GET snapshots the immutable value owner and metadata while holding the Worker mutex, then
copies the public owning value after releasing it. Ordinary hot `prepare_get` does not take the
catalog shared lock. A miss captures the exact Index `RecordRef` and shared generation pin under the
Worker mutex plus a catalog shared lock for pin/manifest identity. The runtime-only Segment visitor
may read up to the fixed physical Segment boundary because this reader's persisted boundary can
predate the active append. It is callable only through that authoritative pinned path. After
positional I/O, decode, CRC, key/hash, sequence, and expiration checks, GET reacquires the Worker
mutex, rechecks the Index reference, and (only when that still matches) takes the catalog shared lock
to confirm the same pin object.

## Alternatives considered

- remove the hot cache: rejected because every durable read would pay file I/O and executor hops;
- global LRU: rejected because recency mutation and one shared lock would recreate cross-Worker
  head-of-line blocking;
- one global atomic byte counter: rejected for the same many-core cache-line contention; static
  Worker partitions make the global bound exact at the cost of temporarily stranded capacity;
- use the stale persisted read boundary: rejected because recently committed active Records would
  become unreadable after eviction;
- copy the value while locked: rejected because mutex duration would scale with value size.

## Consequences

Hot hits retain an in-memory path with one shared-owner increment and an out-of-lock result copy.
Misses and bypasses pay verified positional I/O. Worker partitions can leave capacity unused, and
the conservative allocator-independent accounting can bypass admission earlier than physical RSS
would require. This is intentional: correctness and a stable upper bound take priority over maximum
occupancy. A future cache policy may add Worker-local eviction or a purpose-built flat map without
changing the fallback linearization contract.

## Compatibility and verification

No persistent or wire format changes. Durability and acknowledgement points are unchanged. Tests
cover 0-byte, 64-byte, 4 KiB, and near-1 MiB active values, zero/minimum budgets, hits, overwrite,
erase, TTL, rotation, strict group publication, allocation faults, recovery, and shutdown. ASan,
UBSan, TSan, RSS/hit-rate measurements, and hot/large-value latency benchmarks remain release gates.
