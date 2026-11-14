# Generation slot-pool candidate

**Descriptive** · `src/experimental/generation_slot_pool.hpp` · reviewed 2026-08-27

Decision: [ADR 0036](../adr/0036-generation-slot-pool-publish.md) (proposed).

The experimental, production-congruent candidate for the ADR 0036 generation publication protocol,
and what it proves. The candidate is not selectable by `ShardPairRuntime` or `glifistored`. A
separate production implementation (`include/glifistore/store/paired/generation_slot_pool.hpp`) is
selectable only through the embedded `PairedConcurrencyConfig::generation_slot_pool` option, which
defaults to false; `glifistored` does not set it, and ADR 0036 remains proposed.

## Purpose and containment

`src/experimental/generation_slot_pool.hpp` tests the lifetime protocol that ADR 0036 proposes on
the real immutable `PairReadGeneration` graph. Unlike the complete
[volatile prototype](paired-shard-volatile-prototype.md), it does not reimplement the Index, the
delta or Segments: a slot directly owns the generation produced by production code, including the
pins of durable generations.

The laboratory is compiled only with `GLIFISTORE_BUILD_LAB=ON` (default OFF). The target
`glifistore_lab_tests` owns the candidate tests and the minimal bridge that builds the private
graph in reserved storage; `glifistore_tests` compiles and includes no experimental sources. The
benchmarks `glifistore_generation_shell_benchmark`,
`glifistore_generation_publication_benchmark` and `glifistore_generation_runtime_benchmark` need
the same option and enabled benchmarks. The bridge is not part of the core, is not installed, and
no official path calls it. The production pool remains distinct and opt-in through
`PairedConcurrencyConfig::generation_slot_pool` (default false). ACK, recovery, persistence v1,
wire v2 and the proposed state of ADR 0036 are unchanged.

## Publication

Each pool has a fixed compile-time capacity and one Writer. A slot holds:

- ownership of the graph, `shared_ptr<const Generation>`;
- immutable `epoch` and `visible_through`;
- the Writer-only state `free`, `building`, `published` or `retired`;
- a Writer-only reincarnation counter.

The Writer initializes the complete slot, then publishes a lock-free 64-bit token with release:

```text
bits 63..16  reincarnation epoch
bits 15..0   slot + 1 (zero reserved)
```

The Reader acquire-loads the token, checks the slot's epoch and adopts the raw pointer. Epoch,
`visible_through` and the graph become visible through the same release/acquire edge. A strictly
increasing epoch prevents ABA; exhausting the 48 bits fails explicitly.

## Reclamation and cold I/O

One frontier governs every borrow:

```text
reader_safe_epoch = min(adopted_epoch, minimum_borrowed_epoch)
```

The Reader publishes it with release after finishing the access that adoption requires. The Writer
loads it with acquire and frees only retired slots with:

```text
retired_epoch < reader_safe_epoch
```

The strict comparison keeps the currently adopted generation alive. A cold read started in epoch
`N` keeps publishing `N` during later adoptions; compaction and rotation may publish `N+1`, but slot
`N` is not reset until the I/O completes. Ordinary GET increments no reference count and performs no
atomics: adoption happens once per turn.

The frontier's release orders every Reader access to the old generation before the Writer's reset.
Token monotonicity prevents the Reader from adopting an epoch below the frontier it already
published.

The frontier must be monotonic as well. An attempt to announce, after adoption, a borrow older than
the published frontier is rejected without changing Reader state: that epoch may already have been
reclaimed and cannot be resurrected. A borrow must be registered in the turn in which it starts,
before the frontier advances.

## Boundedness

When no slot is free after opportunistic reclaim, `try_publish` returns `pool_exhausted` without
changing the token or the Writer's current generation. There is no internal wait, growth allocation
or overwrite. Admission, backpressure and the final capacity formula remain the responsibility of
any production integration; the candidate formula is defined here but does not close V9 for the
official runtime.

The production-congruent capacity derives from the runtime's existing normative limit:

```text
slot_capacity = 1 current + maximum_retired_generations
              = 1 + 64
              = 65
```

It does not depend on the mutation queue: the Writer is serial and can have only one `building`
publication. At a debt of 63, the last free slot is reserved before the mutation; after the commit
the `building` slot becomes current and the previous current becomes the 64th retired slot, keeping
the total at 65. At a debt of 64, nothing enters the Store until the Reader advances. A `+2` margin
would therefore be unused memory, not extra safety.

The V9 test holds the Reader still, fills exactly 65 slots, checks that the next attempt is rejected
before the Store, then advances the frontier and proves reclaim and resumption.
`GenerationSlotCapacity<MaximumRetired>` encodes the formula and is compiled against
`ShardPairRuntime::kMaximumRetiredReadGenerations`, so a change to that limit cannot drift silently.

## Reservation and failure after linearization

A publication tied to a PUT or ERASE must reserve a slot **before** it enters the Store mutation:

```text
try_reserve slot
→ Store mutation
→ mark_store_linearized (committed or indeterminate)
→ build generation
→ commit reservation + release publication
→ ACK
```

If `try_reserve` fails, the request is still certainly pre-mutation and can receive an overload
without ambiguity. A reservation abandoned before linearization returns to `free`. After
`mark_store_linearized`, the RAII guard cannot be destroyed without arming the fail-closed hook and
incrementing `unpublished_linearizations`.

The guard does not interpret a plain `ErrorCode`: the caller must mark it for every outcome in which
Store authority may have changed, so at least `committed` and `indeterminate`. The method is
`noexcept` and must be called immediately after Store classification, before any later construction
or allocation.

The durable test forces an invalid publication after an already committed PUT. The guard marks the
Store non-operational and frees the reservation; the snapshot is then acquired with
`allow_fail_closed`, the generation is rebuilt, and the test verifies that both the seed key and the
newly committed authority are present. The candidate emits no success ACK.

## Deterministic shutdown

Stop respects pair ownership. First the Reader stops feeding the mutation lane; then the Writer, on
its own thread, runs `stop_admission()`. That call is the pool's stop linearization: a reservation
that had already observed open admission is admitted and must be committed or cancelled, and every
later attempt is rejected before the Store mutation.

```text
Reader stop mutation admission
→ Writer stop_admission
→ commit/cancel every building reservation
→ drain Reader completions and output/cold I/O
→ mark_reader_quiescent
→ Writer try_finish_shutdown + reclaim
```

`mark_reader_quiescent()` fails while admission is open or a `building` slot exists. The transition
is terminal: it clears the Reader's local pointer, publishes a safe frontier past the last Writer
epoch and prevents new adoptions. The caller may invoke it only after completing every output and
cold-I/O borrow. `try_finish_shutdown()` succeeds only when no reservation remains, the Reader is
quiescent and reclaim has left a single generation: the final published one.

A reservation abandoned during shutdown follows the data-path rule: before linearization it is a
safe cancellation; after linearization it invokes the fail-closed hook. Shutdown therefore never
turns an ambiguous mutation into a silent loss.

The candidate does not model the external SPSC, completion drain or socket close. Those
responsibilities remain required in a `ShardPairRuntime` integration; the pool provides and proves
only the publication and reclamation boundary.

## Generation storage

In the complete candidate, the slot index also selects a preallocated shell storage.
`PairReadGenerationShellBank<Capacity>` prepares a bounded bank together with the pool. The slot
still owns the graph through `shared_ptr`, but `allocate_shared` places the control block, the
`PairReadGeneration` and its embedded `DeltaState` in that fixed storage. After `try_reserve()`, the
Writer uses only the storage with the same `slot_index()`:

```text
reserve slot N
→ build PairReadGeneration in shell_bank[N]
→ Store linearization
→ commit slot N
→ publish token {epoch, N}
```

Each storage accepts one live co-allocation. A second attempt fails with `resource_exhausted`; it
does not fall back to the heap. The memory becomes available again only when both the last
`shared_ptr` and the last `weak_ptr` of the control block are destroyed. The allocator retains the
backing storage, so destroying the external owner cannot cause a use-after-free.

The candidate block is 512 bytes, aligned to 64 bytes. For the normative capacity of 65 slots, the
shell budget is 33,280 bytes per shard, allocated once. A control block whose size or alignment
exceeds the budget fails explicitly: a cross-platform integration needs proof on every supported
toolchain, not an assumption about the `shared_ptr` ABI.

The benefit is narrow. It removes the `malloc` for the shell, control block and `DeltaState` in
incremental publications built in the slot. It does not remove the allocations needed for new
`DeltaPage`s, COW spines, arena blocks, external keys, new bases or pins. It also keeps the
Writer-side reference count; the Reader still uses the raw pointer adopted once per turn.

### Structural inline owner

`PairReadGenerationInlineSlotPool` closes the backing lifetime by construction: it holds the inline
bank before the pool, so reverse destruction order destroys every generation before its bytes. The
builder with a non-owning observer is private, the built generation never leaves the wrapper, and
the Reader receives only the epoch-governed borrow. Allocation, deallocation and counters of the
inline storage are Writer-only and use no atomics.

The wrapper does not accept an external `PairReadMerge`: the merge would keep a `shared_ptr` able to
outlive the bank, so a future integration must also own the merge state in the same domain.
Destroying a storage that is still occupied terminates fail-fast.

The concurrent Writer-publication and Reader-adoption stress covers 10,000 reincarnations under
TSan. The equivalent-protocol microbenchmark measures +7.53% over `shared_ptr`-owned backing, but
stays 7.09% behind `make_shared` without the slot protocol in the same run. The candidate therefore
proves the lifetime is correct, not that the final solution pays off. It is not to be integrated:
the next design must eliminate the control block itself by constructing the generation object in
place.

### Direct-object ring

The first direct-object step reuses the official path's delta validation and construction routine,
then runs `construct_at` on the concrete type directly in two alternating slots. There is no
allocator, control block or `shared_ptr` ownership of the generation. The Writer runs `destroy_at`
before reincarnation; a storage still occupied at destruction terminates fail-fast.

The differential test compares 256 official and direct publications on epoch, visibility, delta,
versions and value. The benchmark measures +17.08% over `make_shared`. This ring has no concurrent
Reader yet: it retires the previous generation synchronously and is therefore not the ADR 0036 pool.
The next sub-block combines the direct object with the token, safe epoch and bounded reclaim, still
keeping any integration separate.

### Direct-object slot pool

`PairReadGenerationDirectSlotPool` completes that sub-block in isolation. Every generation, epoch
zero included, is constructed directly in the inline slots. The pool contains no
`shared_ptr<PairReadGeneration>`, control block or shell allocator. Reservation happens before Store
linearization; publication uses a single release token `{epoch, slot}`; adoption and the safe
frontier are Reader-only; the Writer runs `destroy_at` only for retired epochs strictly older than
the acquired frontier.

Capacity stays compile-time and bounded. Ordinary destruction requires the terminal sequence
`stop_admission → mark_reader_quiescent → try_finish_shutdown`; a storage still occupied, or a pool
destroyed without drain, terminates fail-fast. External merge and snapshot replacement stay
deliberately excluded because they could introduce ownership outside the pool's domain.

The proofs cover saturation without Store entry, physical reincarnation, cold borrow, rejection of a
regressive frontier, fail-closed after an invalid post-linearization build, and 10,000 concurrent
Reader/Writer publications under TSan. The synchronous microbenchmark measures 3,186,299
publications/s: +7.43% over `make_shared`, but −8.28% against the direct ring without the Reader
protocol. This is positive evidence for the protocol, not V11/V12, and it does not yet authorize
integration into the official runtime.

### Two-thread Reader–Writer diagnostic

`glifistore_generation_publication_benchmark` compares the direct pool with the pool of
`shared_ptr` generations, using two distinct persistent threads. Both go through reservation,
publication token, adoption, reclaim and shutdown; only the ownership of the generation differs. The
Reader adopts continuously and verifies the final view, while the Writer publishes 20,000 real
mutations already materialized in the Segment.

The local median is 1,275,537 publications/s direct against 1,190,497 shared: +7.14%, with 6.67%
less total time per publication. Sampled p50 is unchanged at 1,000 ns, but sampled direct p99 is
5.86% worse. The result keeps the throughput direction but does not yet show a tail-latency
improvement.

The affinity request returned `unavailable` on this macOS host. The test proves Reader–Writer
concurrency, not pinning to physical cores. V11/V12 stay open until integration into the
experimental paired path with real GETs and an actually pinned Linux row, plus a macOS row correctly
labeled advisory or unavailable.

The second mode runs a real `PairReadGeneration::get` on every adoption. On the 2-byte L1-hot
dataset, direct reaches 919,357 publications/s against 817,965 shared (+12.40%), improves sampled
publication p50 and p99 by 9.09% and 9.54%, and leaves GET p50/p99 unchanged at 83/250 ns. This
removes the negative p99 signal of the pure adoption storm, but does not replace LLC and
beyond-LLC datasets, realistic values, sockets and verified affinity.

## Evidence

- deterministic exhaustion and recovery after the Reader advances;
- borrow of an exact epoch across publication and reclaim;
- fail-closed rejection of a regressive borrow frontier;
- pre-mutation reservation, safe cancellation and post-linearization RAII fail-closed;
- Writer-owned stop admission, rejection of late reservations and drain of admitted ones;
- normative 65-slot capacity, bounded saturation and recovery after quiescence;
- terminal quiescence rejected while a `building` slot exists;
- slow borrow and a real durable graph retained until the Reader completes;
- final reclaim with a single current generation still owned;
- real snapshot drain after a committed but unpublished durable mutation;
- concurrent stress of 20,000 publications, adoptions and reclaims;
- real durable snapshot after compaction, with a cold read of the old Segment still in flight;
- real durable snapshot after Writer-owned rotation;
- co-allocation of the real generation in fixed storage, deterministic rejection of an occupied
  storage, and reuse of the same address after the last weak owner is destroyed;
- reservation → shell composition with the same slot index, and storage reincarnation;
- inline owner with structural destruction order, no backing ownership or reference count on the
  data path, and a 10,000-publication concurrent stress;
- direct-object slot pool without a generation allocator or control block, with reservation, token,
  safe epoch, cold borrow, bounded reclaim, terminal shutdown and a TSan Reader/Writer stress;
- local microbenchmark: direct pool +7.43% over `make_shared` and −8.28% against the ring without
  the protocol; the worker-affine matrix stays open;
- two-thread diagnostic: direct +7.14% over the equivalent shared protocol, sampled p99 +5.86% and
  macOS affinity unavailable; positive evidence, but not the V11/V12 gate;
- with one real GET per adoption: direct +12.40%, better publication p50/p99 and unchanged GET
  p50/p99; the L1-hot dataset and the lack of pinning keep the gates open;
- focused Release, ASan+UBSan and TSan runs green.

These proofs qualify V5, V6, V8 and V9 as **candidate evidence** and remove the technical blocker
that kept shell allocation outside the candidate. They do not close the production matrix:
integration into the official runtime, lane and socket drain, the crash matrix and, above all, the
worker-affine A/B V11/V12 remain mandatory.
