# Durable runtime catalog

**Descriptive** · persistence v1, all durable Store policies · reviewed 2026-08-26

How the durable backend turns a recovered Store into per-Worker Indexes and a bounded file-backed
runtime, and how that runtime orders mutations, reads, rotation and shutdown. `Store::open` owns it
through the public PImpl for `durable_sync`, `durable_group` and `durable_periodic`.

## Initial bootstrap

Creation publishes a CRC-protected bootstrap intent that holds the complete canonical initial
Manifest, using temporary-file, file-sync, rename and directory-sync ordering. It then publishes the
Manifest, creates one pristine active Segment per Worker, and finally removes the intent and
synchronizes the directory.

A restart may complete missing initial Segments only while this exact intent exists, and only after
verifying that every present Segment is pristine. Without the intent, a missing catalog Segment is
corruption, not an initialization state.

## Open and ownership

`DurableRuntimeCatalog::open_existing` locks the data directory, resolves an interrupted compaction
transaction or completes an exactly validated interrupted rotation, performs full Manifest,
namespace, Segment and Record recovery, moves each recovered Index into its runtime Worker, and
keeps the lock for its lifetime. It then opens one immutable read handle per exact catalog Segment
generation. Generation pins and mutable Segment handles are destroyed before the directory owner.

The only mutating open transitions are the resolution of an interrupted compaction or rotation and
the removal of recognized private temporaries after the complete scan
([recovery implementation](recovery-implementation.md)). Every other invalid namespace and recovery
failure leaves the files untouched and releases the lock.

## Locks and descriptors

Each Worker owns one mutex and at most one mutable active-Segment descriptor. In default paired mode
the per-shard execution token, or the dedicated Writer, is the only mutation owner; the mutex
remains for maintenance, legacy mode, and durable group and periodic state shared with the flush
coordinator. The `durable_sync` exclusive Writer hot path may elide the mutex under the quiescence
protocol.

The catalog owns one immutable shared generation pin, including a read-only descriptor, per
published Segment. Ordinary mutations use only the required shared catalog and publication boundary,
so different Workers proceed concurrently. Rotation takes the exclusive catalog lock, because it
appends to the globally ordered Manifest and commit-state vectors.

Online [durable compaction](durable-compaction.md) performs its replacement I/O and its final
Manifest I/O without holding the Worker or catalog lock. It publishes only if the sequence, batch,
Manifest, source and pin tokens still match; otherwise it durably rolls back the prepared
old-authority transaction. Its logical publication lease makes a second compaction fail fast and
makes rotation wait without keeping the physical serializer during replacement I/O. The waiting
rotation releases the publication mutex, then rebuilds its transition from the newly published
authority. No third Manifest authority can appear while the lease exists.

The steady-state Segment-descriptor bound is the catalog Segment count plus the Worker count. Cold
reads reuse immutable pins, so read concurrency does not raise that bound. On the GET path, catalog
lookup resolves `SegmentId` → pin slot in O(1) through a dense side table rebuilt on recovery,
rotation and compaction publication; identity, generation, owner and pin-object checks remain
mandatory. Other catalog walks may still binary-search the ordered Manifest.

The configured descriptor policy must cover generation pins, Worker mutable handles, directory,
lock, enumeration and transient publication descriptors, and must fit the process `RLIMIT_NOFILE`.
New-key admission uses a deterministic Worker share of the global live-key budget. Rotation
validates the Segment, Manifest, peak-byte and currently available-space budgets before sealing the
old active Segment, so a budget failure is `not_committed` and leaves the existing rotation state
unchanged.

## Verified reads

In paired mode a cold read obtains its `RecordRef` and exact Segment-generation pin from the
immutable published `ReadGeneration`; completion validates that published authority before use. The
legacy and internal mutable-Index path captures the same pair under Worker and catalog
synchronization and revalidates it afterward. In both cases positional I/O, decoding, CRC32C,
sequence, key-hash, full-binary-key, opcode and expiration validation, and the owning value copy run
without a Worker, catalog or file-cache mutex.

A Manifest publication may retire the pathname while an already pinned descriptor stays valid: a
`RecordRef` never crosses a synchronization boundary without generation ownership and an explicit
linearization check. Normal misses and expiration do not poison the runtime; corruption or I/O
disagreement on a still-current pin makes later mutations fail closed.

Durable GET path timing (mutex wait, prepare and complete hold, Index, hot-cache and pin lookup,
cold read, CRC and value copy, relinearization retries) is published through `get_path_stats()` with
relaxed atomics, so the Worker critical section stays short. Fine-grained clock sampling compiles
out of Release builds unless `GLIFISTORE_GET_PATH_TIMING` is set.

## Expiration

After a validated expiration, whether on a generation or cache hit or on a cold Record visit, the
runtime returns `not_found` immediately and never serves the expired value. Physical Index removal
is queued on a bounded per-Worker deferred-TTL backlog and drained by existing Worker paths
(`prepare_get`, `mutate`) with exact `RecordRef` verification, so a concurrent reinsert is never
erased. Matching hot-cache rows are dropped immediately. A zero backlog limit forces synchronous
Index reclaim. Repeated GETs after a successful drain are Index misses and perform no Segment I/O.

Compaction remains the durable TTL cleanup path (`expired_records_dropped`).

## Legacy read path and hot cache

The legacy and internal per-Worker active-Record hot cache is a Swiss-style flat open-addressed
table: power-of-two capacity, maximum load 0.75, geometric growth, 8-slot control groups, H2
fingerprints, SIMD or scalar group matching shared with the Index through `swiss_control_group.hpp`,
and values inlined up to 48 bytes. Default paired opens disable this duplicate read authority and
use only the immutable generation, so operation with no cache stays correct.

Bucket arrays are allocated on the first successful admission, so disabled and never-used Worker
caches have zero table storage. Each resident slot uses its `RecordRef` as its only sequence
identity, derives its inline or heap representation from the value size, and does not duplicate its
recalculable byte charge. Full key bytes are compared on every fingerprint candidate; a hash is
never identity.

The cache is bounded by deterministic shares of a global byte budget, a per-Worker byte cap, a
staging-byte cap and an entry cap. Admission prepares the immutable value before persistent writes;
exhaustion bypasses the cache and never rejects an otherwise valid mutation. Accounting charges the
fixed bucket arrays once, external resident key and value payload separately, and complete temporary
publication state while staged. Resident, staged and bucket bytes, limits, hits, misses, stale hits,
evictions, size rejects, expired-TTL GETs and admission bypasses are observable without a global
cache lock. Rotation removes every entry charged to the retired active generation.

The mutable `prepare_get` compatibility path holds only the Worker mutex; the shared catalog lock is
taken only to acquire a pin on a cold miss. A cache hit snapshots shared immutable ownership under
the Worker mutex and makes the value-sized owning copy after unlocking. On an active-generation
miss, the pinned runtime reader may read past its handle's opening boundary only for the exact
authoritative `RecordRef`; the mandatory post-I/O Index and pin revalidation is its linearization
point.

## Durable mutation order

The owning paired mutation executor, or the legacy Worker lock, serializes these transitions:

1. Prepare Swiss-table capacity, long-key arena storage, bounded hot-cache ownership, and the
   complete encoded Record.
2. Append complete Record bytes.
3. Write the alternate commit slot and apply the synchronization required by the selected policy.
4. Publish the `RecordRef`, advance the Worker sequence, and publish the coherent read generation.

Strict sync and group acknowledgement crosses the required Segment synchronization before success;
periodic acknowledgement may precede the later forced boundary, exactly as persistence v1 specifies.
No allocation is required after the persistent commit. Erase appends a tombstone and uses a
deliberately non-compacting, non-allocating Index removal after commit; volatile erase keeps safe
arena reclamation.

Results distinguish `committed`, `not_committed` and `indeterminate`. Any post-commit publication
error reports the committed boundary and makes the runtime fail closed. While the runtime is
unhealthy, mutations and mutable-Index GETs are rejected; immutable published-generation reads
(`prepare_published_get` and the pin-backed complete) stay servable, so ACK-after-publish
read-after-write holds.

Strict group commit with one v1 Worker separates admission from commit execution. Producers stage
bounded pending mutations and wait for completion; the durability coordinator owns Record ordering,
commit-slot synchronization, whole-batch Index publication and waiter wakeup. An adaptive record
target, bounded by configured minimum and maximum values, closes admission until that batch
completes. A deadline contracts the next target to the observed occupancy; admitted producer
pressure grows it. The first Record still schedules one absolute batch deadline, and an explicit
`flush()` is dispatched to and completed by the same coordinator thread. A multi-Worker Store keeps
independent Worker-local batches and commit domains.

Batch observability lives in a cache-line-aligned Worker-local block that is read without taking the
Worker or catalog mutex. It reports pending Records and bytes, the current adaptive target, flush
attempts and failures, committed occupancy and maxima, close reasons, and total and maximum
`flush_pending_commit` duration. In strict group mode that duration covers the synchronized v1 batch
commit boundary. In periodic or deferred mode it measures deferred commit publication only; the
later whole-store dirty flush is a separate operation and must not be read as included sync time.

For paired strict batches, threshold closes use `SegmentCommitSync::immediate`, and the explicit
Writer finalize commits any residual extent immediately. A deferred threshold publication is not a
strict commit and cannot authorize an ACK.

## Observable shutdown

`Store::close()` first changes the public admission state from open to closing. Calls that already
acquired admission finish; later reads, mutations, verification and flush calls return
`unavailable`. Before waiting, close posts a non-blocking force-all request, so a strict-group
producer cannot deadlock waiting for a long batch deadline. Once every active-call counter reaches
zero, the runtime performs one final force-all flush on the coordinator, stops and joins the
executor, and releases the runtime and the anchored data-directory lock.

Admission accounting is cache-line-isolated per Worker, plus one control shard, so independent
Workers do not contend on a single lifecycle counter during normal operation.

Concurrent close callers observe the same cached result. A final flush or background callback
failure is sticky and makes the runtime fail closed. Destruction invokes close and discards its
status; an application that must tell a clean shutdown from a possible durability failure calls
`close()` explicitly.

## Crash-safe rotation

Rotation first seals the old Manifest-active Segment; that durable lifecycle change is its intent
marker. It then creates the replacement with exactly the Manifest's next ID and generation, and
publishes a new Manifest that marks the old Segment sealed and the replacement active.

At restart, an unlisted Segment is adoptable only if there is exactly one sealed-active marker and
the file has the exact next identity, the same owner, the correct Store ID, the exact size and a
pristine initial active commit (generation 1, zero Records, offset 4096). No arbitrary orphan is
adopted. If the crash preceded creation, open creates the replacement. If the Manifest rename may
already have happened, normal Manifest and namespace recovery selects and validates the completed
state.

## Tests

Tests cover binary reads, expiration, concurrent readers, a blocked cold `pread` that does not block
a same-Worker mutation, blocked compaction build and Manifest-sync concurrency, waiting and
restart-durable unrelated rotation, close and rollback, lock lifetime, sticky corruption handling,
preflighted long-key publication, put, replace and tombstone reopen across restart, sealed-active
completion and exact prepared-replacement adoption.

A separate allocator-interposition executable fails every allocation observed in the native put,
update, erase, owning-read, strict-group and rotation paths. It reopens after every pre-write and
interrupted-rotation failure, requires uncertain paths to fail closed, releases background waiters
after `bad_alloc`, and forbids any allocation after the ordinary Record write boundary through
coherent runtime publication.

Related release gates: native-platform process-kill and power-loss evidence; online compaction
kill/fault matrix completeness; disk-full and native Linux, FreeBSD and OpenBSD rows. See
[production readiness](../production-readiness.md).
