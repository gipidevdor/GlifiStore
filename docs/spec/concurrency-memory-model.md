# GlifiStore Concurrency and Memory Model

Status: normative for the current implementation
Applies to: repository version `0.1.x`
Owner: project maintainers
Last reviewed: 2026-08-27

## 1. Scope

This specification defines thread ownership, synchronization domains, lock order, operation admission, linearization, and shutdown. It does not standardize implementation-private class names, but every shared mutable object must map to one of the domains below.

Product default concurrency is **paired** ([ADR 0032](../adr/0032-paired-concurrency-embedded-store.md),
[ADR 0037](../adr/0037-shard-execution-token-flat-combining.md)): each owner shard has exactly one
mutation executor (execution-token holder), an immutable published `ReadGeneration`, and bounded
combining / async lanes. The deprecated `legacy_mutex` open mode retains the historical per-Worker
mutex path through 0.1.x.

## 2. Threads and executors

An embedded Store has caller threads and, when `async_lane_capacity == 0`, **no dedicated Writer
thread**: callers that win the per-shard execution token become the combiner for that turn
(ADR 0037). When async lanes are enabled (daemon), one dedicated executor thread per shard holds the
same token. Durable mode may add one flush-coordinator thread. The TCP daemon additionally has an
acceptor and one Reader/Reactor thread per shard pair; it is thin I/O over the same Store paired
runtime and must not publish a second generation authority for the same shard. The Reactor never
executes Store mutate. A dedicated Writer drains at most 32 synchronous records before an
already-admitted async turn; any older remainder stays in a Writer-local FIFO continuation ahead of
new sync admissions. This bounds a single large caller batch without changing its internal FIFO
order. Durable-group collection must also stop at the oldest non-zero pre-Store queue deadline, even
when its durability batching deadline is later.

Store callers may invoke public operations concurrently unless a method explicitly says otherwise.
Thread safety does not create multi-key atomicity. Concurrency mode is fixed at open, so the public
API provides no supported operation that mixes `legacy_mutex` and paired mutation ownership. Any
detected internal ownership violation fails the Store closed.

## 3. Ownership table

| State | Owner / synchronization | Notes |
|---|---|---|
| Paired published `ReadGeneration` | immutable after Writer release | ordinary paired GET adopts with acquire; no Index mutex |
| Paired mutable Index / active Segment / delta | holder of that shard's execution token | sole mutator; other callers enqueue for the combiner |
| Volatile Worker Index and segments (`legacy_mutex`) | that Worker's mutex | ordinary key operations acquire exactly one Worker mutex |
| Durable Worker Index, active file, hot records, group state (`legacy_mutex`) | that durable Worker's mutex | never accessed concurrently without the mutex |
| Durable hot cache (paired) | disabled / not ordinary-read authority | generation-only policy; compaction/verify/backup still use catalog locks |
| Volatile global segment namespace | `GlobalSegmentManager` mutex | snapshots are copied while locked |
| Durable segment catalog | catalog shared mutex | readers take shared; namespace mutation takes exclusive |
| Manifest publication | manifest-publication mutex + condition variable | serializes generations; rotation waits only for the post-staging compaction lease |
| Public compaction | Store compaction mutex | only one compaction attempt runs at once |
| Store lifecycle admission | sharded atomic counters | one shard per Worker plus one control shard |
| Flush scheduling state | coordinator mutex and condition variables | callback executes after releasing this mutex |
| TCP connection | exactly one Reader/executor | ownership may transfer once after bind |
| Paired sync combining queue | LIFO admit / FIFO combine under token | ≤32 records per dedicated turn; older Writer-local continuation precedes new sync admissions; no wait-to-fill |
| Paired async mutation / completion lanes | bounded SPSC | one Reader (or embedded submitter) producer, one dedicated executor consumer |
| Executor handoff queue (connection bind) | bounded MPSC protocol | many producers, one owning consumer |
| Directory health | atomic state plus protected error payload | acquire/release publishes terminal health |

No reference, iterator, or span into a protected container may escape after its lock is released unless the referenced object has independent shared ownership and immutable lifetime guarantees. Snapshot APIs therefore return copies such as `std::vector<SegmentPtr>` and `std::vector<SegmentId>`.

## 4. Worker routing

The complete key is hashed before selecting the owner shard. The selected Worker/shard is stable for the duration of the operation because worker count and routing epoch are immutable for an open Store. A Store operation must not consult a second Worker to recover from a miss. A miss in the owning Index or published generation is authoritative for the current in-memory state.

## 5. Lock order

When more than one lock is necessary, acquire locks only in these orders:

1. public compaction mutex;
2. Worker / RuntimeWorker mutex (legacy path, or paired maintenance/snapshot/refresh that still takes it);
3. manifest-publication mutex;
4. durable catalog mutex.

Paired ordinary `get` / `put` / `erase` do not acquire the Worker Index mutex on the hot path when
the shard has an exclusive Writer and no background durable flusher. Compaction, verify, backup,
and durable catalog refresh may still take Worker and catalog locks as today. Durable mutate still
takes a catalog shared lock to pin the active generation; group/periodic modes retain the Worker
mutex while a flush coordinator shares pending-batch state.

For the volatile runtime, the allowed nested order is Worker mutex, then global segment-manager mutex. Code that holds the segment-manager mutex must not acquire a Worker mutex.

Verification that must observe all Workers locks Worker mutexes in ascending Worker identifier order. No ordinary key operation locks all Workers.

The flush coordinator's internal mutex is not part of this storage lock order: it protects request/generation state only. Its callback is invoked with that mutex released, preventing condition-state waits from enclosing filesystem or Worker work.

A rotation that reaches a compaction publication lease snapshots its expected Worker sequence, then
waits on the publication condition variable while retaining neither the publication mutex nor its
owning Worker mutex. Other mutations on that Worker may therefore progress. After wakeup, the
rotation reacquires the Worker and publication locks in the prescribed order and validates the
sequence before constructing a transition from the newly authoritative Manifest. Sequence drift
returns the exact `not_committed + sequence_conflict` outcome; the daemon may retry that owned task
once. The rotation must not publish a third authority while the dual-Manifest compaction intent is
active.

Compaction source generation pins and an owning Index snapshot are captured while the Worker/catalog
state is synchronized. All source reads and replacement writes thereafter use those immutable pins
or compactor-private staged file handles. The logical Manifest lease is acquired only after every
staged output is sealed and verified, immediately before durable intent publication. No `RecordRef`,
file handle or Segment reference is dereferenced after dropping its synchronization unless such a
pin/private owner remains live. This shortens the catalog exclusion window without substituting an
equivalent global lock around copy I/O.

Rotation telemetry adds no lock to this order. The runtime records the time to acquire publication
authority and wait for the compaction lease separately from Segment seal, replacement creation,
Manifest publication, aggregate rotation execution, and the final Record commit using atomics. A
short compare-and-exchange writer gate serializes only updates to these atomic counters; versioned
publication keeps completed duration aggregates coherent for readers without holding the Manifest
serializer, catalog mutex, or a Worker mutex. Snapshot reads remain available while a rotation is
blocked on the compaction lease.

New nested locking requires updating this document and adding a test or static invariant that makes the order reviewable.

## 6. Operation admission and shutdown

Admission uses one cache-line-separated atomic counter per Worker plus a control counter. The high bit means admission closed; remaining bits count admitted operations.

An operation:

1. increments its shard;
2. detects the closed bit;
3. if closed, immediately decrements and fails;
4. otherwise completes all access to Store-owned runtime state;
5. decrements the shard and notifies that closer when it was the last admitted operation.

Closing atomically sets the closed bit on every shard, then waits directly on each atomic counter
with acquire observations until its count reaches zero. The last admitted operation publishes the
zero transition and notifies that same atomic. The drain must not use a condition variable whose
predicate changes outside its mutex: that creates a check-to-sleep lost-wakeup window. Consequently,
destruction of runtime objects happens after every previously admitted operation has released them.
The lifecycle transition is one-way. Repeated `close()` calls wait directly on the atomic lifecycle
state and observe the first close result after its protected error payload is published.

In paired mode, `close()` also stops Writer admission and drains every admitted mutation before
tearing down generations and Writer threads.

No operation may retain a raw pointer into the Store after releasing its admission token.

## 7. Linearization points

| Operation | Linearization point |
|---|---|
| Paired `get` | validation of the mapping in the adopted `ReadGeneration` (after acquire of the published pointer) |
| Paired `put` / `erase` | Writer publication of the new generation that includes the mutation (release), after the Store mutate has committed in-process |
| Volatile `get` (`legacy_mutex`) | read and validation of the current Index mapping while holding the owner mutex |
| Durable `get` (`legacy_mutex`) | selection and validation of the record identified by the owner Index while holding the owner mutex |
| `put` (`legacy_mutex`) | publication of the new `RecordRef` in the owner Index, after the complete record exists |
| `erase` (`legacy_mutex`) | removal/tombstone publication in the owner Index, after any required durable record exists |
| `flush` | completion of the requested flush generation or synchronous persistence boundary |
| `compact` | publication of the accepted replacement namespace; cleanup may follow |
| `close` | successful transition that closes admission; completion additionally waits for drain and teardown |

A successful `put` may be visible before it is crash-durable in relaxed or group-commit modes. The exact acknowledgement point is defined by the durability policy, not by mutex release alone.

## 8. Atomics and memory ordering

Memory orders are chosen for a specific publication relation:

- admission count arithmetic may be relaxed, while close/drain uses acquire/release operations to order teardown after admitted work;
- terminal health and lifecycle publication use release stores and acquire loads so observers see the associated state;
- paired `ReadGeneration` pointer publication uses release store by the Writer and acquire load by Readers;
- the per-lane current-generation memory census is telemetry, not object publication: the sole Writer
  brackets relaxed component stores with an odd/even sequence, and observers retry around acquire
  sequence loads until they see the same even value. No census field may be used to authorize
  reclamation, visibility, admission, or dereference;
- queue cell sequence numbers use release publication and acquire consumption; enqueue/dequeue positions may use relaxed arithmetic because cell sequence is the handoff barrier;
- the bounded compaction journal has one serialized Index publisher. Its sticky overflow flag uses
  release/acquire to report exhaustion; the commit gate must separately drain publishers before
  reading non-atomic journal entries or replaying them. The publisher's relaxed overflow check does
  not grant the compactor access to those entries;
- owning blob reads reserve a self-contained aggregate byte charge with atomic compare/exchange
  before allocating a materialized value. The initial relaxed load is only a reservation candidate;
  the counter does not publish value bytes or replace the Store admission/lifetime protocol;
- counters used only as statistics, high-water marks, or scheduling hints may be relaxed and must never be used to publish object contents.

Changing an atomic's role from telemetry to correctness requires revisiting its memory order and this specification. `volatile` is not a synchronization primitive.

## 9. MPSC connection handoff

The server's bounded queue has many producer executors and one consumer executor. A producer constructs a complete handoff object, claims a cell, and publishes the cell with release semantics. The sole consumer observes the cell with acquire semantics before taking ownership.

After successful publication, the producer must not read or mutate the connection. Before successful publication, the consumer cannot observe it. If the queue is full, the producer retains ownership, may emit wire `OVERLOADED` for the pending `BIND_WORKER`, and then closes the connection; the server does not create an unbounded backlog. After successful publication, destination connection-table exhaustion or adopt registration failure likewise best-effort `OVERLOADED` then close (the producer cannot restore). Wakeups are hints to run the consumer, not ownership transfer by themselves.

This queue is for one-time connection transfer. It is not a general per-request remote execution mesh.

## 10. Durable flush coordination

The coordinator accepts ordinary, all-worker, and deadline-based flush requests. Its mutex protects flags, deadlines, generations, and terminal background error. `flush_all_blocking()` serializes callers, records a generation, and waits until that generation completes or the coordinator stops.

The callback may acquire storage locks and perform I/O, so it always runs without the coordinator state mutex. A callback failure is recorded as terminal background error, wakes waiters, and stops the coordinator. `stop()` is serialized, requests thread stop, wakes all waiters, and joins the thread.

## 11. Data-race obligations

For every mutable field, at least one must be true:

- it is thread-confined;
- every access is protected by the same mutex;
- it is atomic with a documented publication role;
- it is immutable after publication and its lifetime is independently owned.

Tests under ThreadSanitizer are evidence, not a substitute for these invariants. A passing race detector does not prove absence of deadlocks or incorrect relaxed ordering.

## 12. Memory-footprint invariants and accounting

The immutable base separates lookup metadata from records. Every allocated bucket owns one control
byte and one `uint32_t` record index; every live base row owns one 64-byte compact record containing
its full hash. A control/fingerprint match is never authoritative by itself: lookup confirms the
full hash and key bytes before returning a `RecordRef`. This preserves exact lookup semantics while
removing the former 64-bit hash and pointer pair from every bucket.

Base-record arrays of at least 1 MiB use private anonymous mappings in geometric size classes, with
an inaccessible guard page at each end, on the supported POSIX targets. Each immutable-base lineage
owns a synchronized pool with at most one retired spare mapping. A same-class rebuild reuses that
spare; a class change replaces it; destruction of the lineage unmaps it. The pool mutex is never
read or acquired by GET and is touched only by base-array allocation or retirement. This prevents a
general-purpose allocator from retaining every successively larger merge result without restoring
zero-fill/page-fault cost on every merge. The policy is per lineage rather than per calling thread,
so embedded flat-combining callers cannot multiply retained mappings. Ownership, publication,
lookup semantics, and the persistent format are unchanged. Smaller arrays remain ordinary vector
allocations. `base_record_mapped_storage_bytes` separates the current mapped payload from
allocator-owned payload; process-wide `read_generation_spare_mapping_bytes` accounts the bounded
spare payload.

ASan builds deliberately fall back to the instrumented general allocator so intra-array redzones
and use-after-free detection remain available; production hardening retains the mapping guard pages.

`ReadGenerationMemoryStats` describes only the currently published generation. Byte fields are
allocation-payload lower bounds: they include explicitly sized base/delta arrays, arenas, key
blocks, pins, and the generation shell, but exclude allocator/control-block overhead. The census
does not sum retired generations or an in-progress merge builder because their persistent
structures can share allocations with the current generation and a naïve sum would double-count.
Process RSS and peak RSS are therefore separate measurements, and the residual between attributed
payload and RSS must remain visible rather than being presented as per-key memory.

Neither the census nor the compact layout changes the hard bounds on delta size, retired generation
count, merge post-cut capacity, mutation lanes, or connection buffers.

An incremental base merge is scheduled against those bounds rather than by an unqualified fixed
retry count. Let `R` be the exact remaining initialization/base/delta slots, `P` the smaller of
remaining distinct-entry and record-version capacity in the post-cut delta, and `N` the maximum
records in the next publication. Before that publication the sole Writer advances at least
`ceil(R*N/P)` slots (or all `R` when `N >= P`), subject to a configured minimum work quantum. A
coalesced publication may amortize one additional minimum quantum, but client batch size cannot
linearly widen the maintenance turn. Phase transitions consume no slot budget. This invariant
keeps post-cut memory bounded and prevents delayed merge debt from becoming a terminal retry burst
or spurious pre-Store overload. It does not change publication, ACK, or Reader adoption ordering.

## 13. Failure and cancellation

Exceptions must not cross background-thread boundaries. Allocation failure becomes `resource_exhausted`; unexpected failure becomes `internal_error`. Network disconnect cancels only that connection's future requests; it does not roll back already linearized Store operations.

The current API has no general operation cancellation token. Shutdown waits for admitted operations and filesystem work; it does not forcibly invalidate objects still in use by those operations.

## 14. Review checklist

Any concurrency change must answer:

1. Which thread or lock owns each new mutable field?
2. Does it introduce a new nested lock acquisition?
3. What is the linearization point?
4. What publishes initialized data to another thread?
5. What happens if the queue is full, allocation fails, or shutdown begins?
6. Can a reference outlive the lock or owner that protects it?
7. Which deterministic and sanitizer tests cover the change?
