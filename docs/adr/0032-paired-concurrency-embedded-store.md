# ADR 0032: Paired concurrency for the embedded Store

- Status: accepted
- Date: 2026-07-31
- Deciders: project owner, storage, networking, performance e reliability maintainers
- Applies to: embedded `Store::open` / `get` / `put` / `erase` and `glifistored` thin I/O
- Amends: [ADR 0031](paired-reader-writer-shards.md), [ADR 0005](0005-worker-auto-sizing.md),
  [ADR 0009](0009-public-read-ownership.md) (concurrency notes only; owning `get` unchanged)
- Amended by: [ADR 0037](0037-shard-execution-token-flat-combining.md) (execution token + flat
  combining; embedded sync omits mandatory Writer thread)
- Supersedes: Worker-mutex serialization as the product default for public Store key operations

## Context

ADR 0031 made Reader/Writer shard pairs the mandatory runtime for `glifistored`, with GET served
from an immutable `ReadGeneration` and a single serial Writer per owner id. The embedded `Store`
API still linearized ordinary `get` / `put` / `erase` on per-Worker mutexes
(`Worker::mutex_` / `RuntimeWorker::mutex`). The daemon Writer mutated through
`detail::StoreAccess`, then published a second generation authority outside the Store. That left
two product concurrency stories and duplicated publication.

Persistence format v1, wire protocol v2, routing seed/owner id, and owning public `Store::get`
(ADR 0009) are out of scope for this decision.

## Decision drivers

- One concurrency model for embedded Store and daemon.
- Ordinary GET without acquiring the Index mutex when a published generation is available.
- Exactly one mutator per shard; multi-thread `put`/`erase` serialize on the Writer lane, not on
  the Index mutex.
- Bounded SPSC publication already proven in the daemon paired path.
- Refuse ambiguous dual-mutator configurations at open time.
- No Manifest/Segment/Record byte change; no E3 certification claim.

## Alternatives considered

### Keep mutex Store; paired only in the daemon

Rejected: perpetuates two linearization stories, keeps double publication, and blocks embedding the
same p99 GET-under-write properties outside TCP.

### Multi-reader / `shared_mutex` / RCU on the Index

Rejected for 0.1.x: larger memory-model surface, harder shutdown and pin accounting, and not required
once a published immutable generation exists.

### Zero-copy public `Store::get`

Deferred under ADR 0009. This ADR does not change the owning-read contract.

## Decision

1. **Default `Store::open` concurrency is paired.** Each owner Worker/shard has exactly one
   **mutation executor** (holder of the per-shard execution token — [ADR 0037](0037-shard-execution-token-flat-combining.md)),
   one immutable published `ReadGeneration`, and bounded mutation/completion structures owned by
   `ShardPairRuntime` inside `glifistore_core`. Embedded sync (`async_lane_capacity == 0`) does
   **not** require a dedicated Writer `std::thread`; the caller that wins the token is the combiner.
   Daemon lanes (`async_lane_capacity > 0`) retain a dedicated executor thread.
2. **Public API:**
   - `get` / `get_copy` adopt the published generation and return `OwnedValue` (ADR 0009 unchanged).
   - `put` / `erase` may be called from multiple threads; they acquire the shard token or enqueue for
     the current combiner and wait for completion. Same-shard callers serialize on the token /
     combining queue, not on the Index mutex.
3. **Compatibility escape hatch:** `StoreConfig::concurrency = StoreConcurrencyMode::legacy_mutex`
   restores the historical Worker-mutex path. It is deprecated in 0.1.x and removed in 0.2.
   Opening a Store that mixes legacy mutex mutators with a paired Writer on the same instance is
   undefined behavior and must be refused at open (or fail closed if detected later).
4. **Hot cache:** in paired mode the generation is the sole ordinary-read authority. Durable hot
   cache admission is disabled by default for paired opens (generation-only) so Index+hot and
   generation cannot disagree.
5. **`glifistored`:** becomes thin TCP/TLS/UDS I/O over the same Store paired runtime. It must not
   maintain a second publication spine for the same shard. Mutation windows / GET visibility barriers
   are specified under ADR 0037 Phase C.
6. **Experimental** `src/experimental/paired_*` remains lab/microbench only and is not a selectable
   product runtime.
7. **Immutable base layout:** the Reader-only base stores one 64-byte compact record per live row,
   including the full 64-bit hash, and a separate lookup table made of one control byte plus one
   32-bit record index per bucket. Fingerprint matches are always confirmed with the full hash and
   full key. This is an implementation-private representation: persistence v1, wire v2, routing,
   visibility, pin lifetime, and public outcomes do not change.

Worker count, routing algorithm/seed, and Manifest `worker_count` remain the persisted ownership
identity; “shard pair count” is the runtime view of that same count (ADR 0031 / 0030).

## Consequences

### Positive

- Embedded and daemon share one Reader/Writer contract and one generation authority.
- Ordinary GET can avoid Worker/RuntimeWorker mutex acquisition on the hot path.
- Daemon double-publish is removed once the thin-I/O collapse lands.
- Uncontended embedded PUT avoids cross-thread park when the caller holds the token (ADR 0037).

### Negative / costs

- Contended same-shard embedded `put`/`erase` latency includes combiner-queue wait.
- Legacy mutex path must be kept until 0.2 for callers that explicitly opt in.
- Maintenance, compaction, verify, backup, and durable catalog refresh retain their existing locks.
- The current-generation allocation census is a lower bound, not total process memory: retired
  generations, an in-progress merge builder, allocator metadata/caches, thread stacks, and unrelated
  runtime allocations require separate RSS/allocation evidence.
- Immutable-base record arrays at or above 1 MiB bypass the general allocator. One geometric
  size-class spare is owned by each base lineage and reused only during merge allocation/retirement;
  the pool lock is absent from GET and ordinary delta publication. This bounds retained merge-array
  residency without making the cache depend on whichever embedded caller won the execution token.

### Risks

- Callers that assumed mutex fairness may see different latency under write burst; mitigate with
  release notes, queue metrics, and the deprecated `legacy_mutex` flag through 0.1.x.

## Compatibility and migration

| Surface | Effect |
|---|---|
| Persistence v1 / Manifest / Segment / Record | Unchanged |
| Wire protocol v2 / SDK daemon clients | Unchanged |
| Public owning `Store::get` | Unchanged (ADR 0009) |
| Default `Store::open` | Paired concurrency |
| `StoreConcurrencyMode::legacy_mutex` | Deprecated 0.1.x; removed 0.2 |
| CI default | Paired; legacy only via explicit job/config |

## Verification

- Correctness: existing suites plus GET∥PUT same key, close drain, read-after-write; crash E2
  unchanged.
- Sanitizers: ASan/TSan/UBSan green on paired default.
- Persistence: migrate/reopen/worker seed byte-identical.
- Performance: GET p99 under write burst no worse than current daemon paired; embedded
  single-thread median regression ≤ 5%.
- Layout/accounting: unit tests pin the 64-byte record and 5-byte-per-bucket lookup payload; server
  integration tests verify a coherent per-lane current-generation census and bounded STATS fields.
- API docs and examples updated; no E3 certification language.

## References

- [ADR 0031 — paired Reader/Writer shards](paired-reader-writer-shards.md)
- [ADR 0037 — shard execution token and flat combining](0037-shard-execution-token-flat-combining.md)
- [ADR 0009 — owned public reads](0009-public-read-ownership.md)
- [Concurrency and memory model](../spec/concurrency-memory-model.md)
- [Worker model](../architecture/worker-model.md)
- [Public API contract](../architecture/public-api-contract.md)

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
The opt-in durable v2 embedded profile uses the same paired owner and execution-token protocol for descriptor mutations. Blob preparation occurs before admission; legacy_mutex and daemon blob enablement are unsupported.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
