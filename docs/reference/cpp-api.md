# Public C++ API

**Normative** · `glifistore::Store` 0.1.x · reviewed 2026-09-15

The supported C++ surface of the embedded Store, normative for the current installed public headers
while the product remains below alpha. It settles what each operation accepts and returns, who owns
which bytes and for how long, what may run concurrently and what stays compatible.

## Surface

The public API is declared under `include/glifistore`. The Store header uses PImpl, owning reads and
a deliberate installed header set. The supported embedded API contains:

- Store creation, open and close;
- byte-key `get`, `get_copy`, `put`, `put_batch`, `erase` and `erase_batch`, with owning result
  values;
- configuration value types and typed error categories;
- explicit flush, fenced backup and compaction;
- maintenance snapshots and Index verification.

All three durable policies use the same public key operations as volatile mode. Store-owned clock
injection, positional `put_batch` and fenced online backup are implemented; a dedicated
incompatible-format error and pinned zero-copy reads remain future additive work.

Workers, Index partitions, Segments, Record codecs, routing hashes, pollers, Reactors and vacuum
builders are implementation mechanisms. They are not supported merely because a header is present
under `include/`, and implementation-private classes under `src/` are not public API merely because
tests can include them. CMake packaging installs a deliberate public header set and keeps internal
headers out of exported target usage requirements; additions require an explicit API decision.

The native daemon and wire protocol have their own compatibility contract. Internal server paths may
call owner-checked Store operations without exposing those operations to embedded consumers. The
separately installed [C ABI v1](../spec/c-abi-v1.md) is a smaller synchronous facade; it does not
make C++ layouts or the full Store surface ABI-stable.

Fallible operations return `Result<T>` or `Status`, based on `std::expected`. Input spans are
borrowed only for the duration of the call; the Store never retains them.

## Open

```cpp
static auto open(const StoreConfig& config = {}) -> Result<std::unique_ptr<Store>>;
```

`open` validates the configuration, selects the volatile or durable runtime and completes recovery
before it returns success. On failure no usable Store is published. Durable open may inspect and
safely complete a documented recovery intent, but must not mutate an incompatible or ambiguous
namespace.

Durable mode requires `data_directory`. `create_new` rejects an existing leaf, `open_existing`
requires durable metadata, and `open_or_create` initializes only a missing or pristine directory.

The Worker count becomes fixed for the returned Store, and configuration is immutable after open.
Worker auto-sizing applies only to creation. Durable reopen uses the persisted routing metadata and
rejects incompatible explicit configuration: an explicit Worker count on reopen must match it.

```cpp
auto worker_count() const noexcept -> std::size_t;
```

`worker_count()` returns the immutable Worker count. It stays valid after close, performs no
operation admission and says nothing about current availability.

### Durable resource policy

`StoreConfig::durable_limits` is immutable runtime policy, not persistent metadata. It bounds total
and peak Store bytes, the free-space reserve, Segment count, Manifest bytes, Store-owned
descriptors, recovery memory, live keys, temporary compaction bytes and write amplification. The
same v1 Store may therefore be opened under a different policy, but a policy smaller than its
existing catalog or recovery state rejects open without editing the Store.

The policy also retains bounds for the derived durable hot cache, globally and per Worker, with
separate entry and pre-publication staging limits. Default paired opens disable that duplicate read
authority and use immutable generations; these limits remain relevant only to the deprecated
`legacy_mutex` path and to 0.1.x configuration compatibility. Zero-cache operation never changes
durable data or mutation success.

The live-key limit is partitioned deterministically across the persisted Workers, and the partition
sizes sum exactly to the configured total. This preserves Worker-local admission and avoids a shared
counter on the mutation path. A new key returns `resource_exhausted` when its owner partition is
full; replacement remains permitted, and a committed erase returns its capacity.

Bootstrap and rotation check the configured peak namespace bytes and the currently available
filesystem space before publishing an intent or sealing an active Segment. The free-space sample is
advisory in the presence of other writers and thin provisioning; fixed-size native Segment
preallocation remains the authoritative allocation boundary.

## Keys, values and limits

Keys and values are arbitrary, uninterpreted byte strings: no text encoding or normalization is
performed. Outside the opt-in blob profile (see [Blobs](#blobs)), empty keys are valid and public
operations validate that the encoded Record, including headers and alignment, does not exceed the
normal 1 MiB Record limit.

Callers do not provide a precomputed routing hash. The Store hashes the exact key bytes once and
uses that result consistently for routing, Record metadata and Index lookup, so a caller-supplied
key/hash mismatch cannot violate Worker ownership.

### Expiration

`expire_at_ns` is an absolute Unix-epoch timestamp in nanoseconds; zero means no expiration. A value
is expired when the Store clock is greater than or equal to its nonzero expiration. Expiration
removes visibility, but durable byte reclamation remains compaction work.

Production calls use the Store clock. Deterministic tests inject a thread-safe, non-throwing
`StoreClock` through construction rather than passing arbitrary time into each public read. With no
injected clock, the Store reads `system_clock` and clamps values before the Unix epoch to zero and
values beyond the encoded range to the `uint64_t` maximum.

Each Store instance maintains an atomic high-water mark, so a backward wall-clock adjustment cannot
make time move backward during that instance's lifetime. Recovery samples the same clock once and
uses that snapshot for its complete scan; later reads advance from that value. A process restart
cannot preserve the high-water mark without adding persistent state, so operators who require strict
TTL behavior across large clock corrections must keep the host Unix clock synchronized and
nondecreasing.

## Reads and ownership

```cpp
auto get(std::string_view key) -> Result<OwnedValue>;
auto get(std::span<const std::byte> key) -> Result<OwnedValue>;
auto get_copy(std::string_view key) -> Result<OwnedValue>;
auto get_copy(std::span<const std::byte> key) -> Result<OwnedValue>;
```

`get` returns an owning copy of the latest visible byte value and its sequence and expiration
metadata. An absent key, a tombstone or an expired latest Record returns `ErrorCode::not_found`;
absence is not an optional successful value.

`get_copy` is the current compatibility spelling of the same owning read, not a zero-copy API: it
does not select a distinct slow or zero-copy path.

The returned `OwnedValue` owns its bytes and metadata; no borrow of Store memory escapes the call
([ADR 0009](../adr/0009-public-read-ownership.md)). **Returned bytes and metadata remain valid
after later Store calls, mutation of the same key, vacuum, close and Store destruction.** This is
the safe default and the source-compatible API on which ordinary consumers should rely.

A pinned read handle is a reserved future extension and is not part of the current public API. A
future pinned read must use a distinct handle that owns Segment lifetime and must not change the
ownership of these methods. A zero-copy API may return a move-only `PinnedValue`:

- Its immutable key and value views remain valid until the handle is destroyed.
- A pin holds the exact Segment generation resident and prevents unmap, deletion, reuse or
  destructive repair.
- Replacing or erasing the key does not change the bytes observed through an existing pin. The
  handle is a historical read snapshot, not a promise that the key remains current.
- The handle may be moved between threads and destroyed on a different thread, but concurrent
  mutation of the handle object itself is not allowed.
- Store close stops new operations but does not invalidate an existing pin. Reclamation must account
  for pins independently of the Store object's lifetime.

Non-owning `RecordView` results are restricted to internal code whose lexical execution scope pins
the Segment or otherwise proves that reclamation cannot occur. They must not cross an asynchronous
boundary or be stored in connection state.

## Writes

```cpp
auto put(std::string_view key, std::span<const std::byte> value,
         std::uint64_t expire_at_ns = 0) -> Status;
auto put(std::span<const std::byte> key, std::span<const std::byte> value,
         std::uint64_t expire_at_ns = 0) -> Status;
```

`put` publishes the byte value, with its optional expiration, for the complete binary key. The input
is copied or durably encoded before return and need not outlive the call. Success does not expose
the internal `RecordRef` or replacement status. The durability guarantee depends on the storage
mode; [durability and recovery](../architecture/durability-recovery.md) defines each mode.

## Batches

```cpp
struct Store::PutItem {
    std::string_view key;
    std::span<const std::byte> value;
    std::uint64_t expire_at_ns;
};

auto put_batch(std::span<const PutItem> items) -> std::vector<Status>;
```

`put_batch` accepts a borrowed positional list and returns one status per input item. Items may span
owners: the Store groups them by owner, each owner serializes and publishes its own FIFO subsequence
independently, and the returned vector restores caller order. A batch is not a cross-key
transaction: owners publish independently, and a failure on one item does not roll back a successful
sibling.

Every successful item is visible when the method returns. Within one owner, each read publication
covers at most 32 items, and a dedicated paired Writer processes at most 32 synchronous items per
scheduling turn. On a dedicated paired Writer, concurrently admitted asynchronous mutations may
linearize between those turns, while the batch's own items keep their FIFO order. Key and value
spans are borrowed only for the call.

## Erase

```cpp
auto erase(std::string_view key) -> Status;
auto erase(std::span<const std::byte> key) -> Status;
```

`erase` removes current visibility for the binary key. Durable mode records a tombstone when
required for recovery ordering. An absent or expired key returns `ErrorCode::not_found`.

```cpp
struct Store::EraseItem {
    std::string_view key;
};

auto erase_batch(std::span<const EraseItem> items) -> std::vector<Status>;
```

`erase_batch` follows the same positional, per-owner, non-transactional rules as `put_batch`. Key
views are borrowed only for the call, and successful erases are visible when the method returns.

## Blobs

```cpp
auto begin_blob_put(std::string_view key, BlobOptions options = {})
    -> Result<std::unique_ptr<BlobWriter>>;
auto open_blob(std::string_view key) -> Result<std::unique_ptr<BlobReader>>;
auto collect_blobs() -> Result<BlobGcResult>;
```

These calls belong to the opt-in blob profile of [ADR 0042](../adr/0042-large-blob-storage.md);
[large blob storage v1](../spec/large-blob-storage-v1.md) is its normative contract.
`StoreConfig::enable_blobs` selects the profile on a durable paired embedded Store without an
asynchronous lane, and open rejects any other configuration with `invalid_argument`. The profile
uses persistence family v2 and never upgrades a v1 Store. Without it, all three calls return
`unavailable`.

- `begin_blob_put` copies the key and returns an owning `BlobWriter`. `write()` stages bytes without
  changing visibility, only `commit()` publishes the value, and destroying an unfinished writer
  aborts the upload.
- `open_blob` returns an owning `BlobReader` pinned to the value visible when it opened; a later
  replace or erase does not change the stream. `close()` makes further transfer I/O fail.
- `collect_blobs` flushes, captures catalog roots and removes unreferenced blob files. It runs only
  when called.

In the profile, every key must be nonempty and at most 1,048,440 bytes, and a value may reach
64 MiB. `put` stores a value above the inline limit as a blob, and `get` and `get_copy` materialize
it into the owning result; concurrent materializations share a 64 MiB budget and fail with
`resource_exhausted` beyond it. The profile is a prototype: the gates in
[`engineering/gates/large-blobs.yaml`](../../engineering/gates/large-blobs.yaml) are open
(`GATE-BLOB-EMBEDDED` is `IMPLEMENTATA`, `GATE-BLOB-WIRE` is `NON_INIZIATA`), and the daemon, wire
protocol, C ABI and SDKs do not transfer blobs.

## Flush

```cpp
auto flush() -> Status;
```

In durable modes, `flush()` waits until all mutations covered by the call have crossed the mode's
required persistence boundary, or returns an error. Later concurrent mutations need not be covered.
A flush failure may poison the durable runtime and make later operations fail closed. In volatile
mode, `flush()` succeeds as a no-op.

## Online backup

```cpp
auto backup_to(const std::filesystem::path& destination, bool scan_records = true)
    -> Result<DurableStoreBackupReport>;
```

`backup_to` creates a verified persistence-v1 catalog copy while the durable Store remains open. The
destination must be new and empty. It is available only for durable Stores: volatile Stores reject
it with `invalid_argument`.
[Backup and restore v1](../spec/backup-restore-v1.md) is the normative boundary.

The call briefly fences new admissions, drains and flushes admitted work, structurally verifies the
catalog and copies it under the exclusive catalog boundary, writes the destination Manifest last and
resumes admissions. It then optionally performs the complete destination Record CRC scan. Concurrent
backup callers retain independent counted fences, so completion of one copy cannot reopen admissions
underneath another copy waiting for the catalog boundary.

This is the online **fenced** contract, not a zero-fence concurrent hot snapshot. An incomplete or
failed destination must not be promoted; any failed destination remains unfit for service until an
independent verification succeeds.

## Maintenance and compaction

```cpp
auto compact() -> Result<CompactionResult>;
```

`compact()` runs explicit compaction according to the storage mode's selection policy; selection for
a single call is round-robin across Workers. Durable mode runs one crash-consistent transaction
that replaces the oldest sealed prefix of the selected Worker: the first `k` of its `n` sealed
Segments in Manifest order, 1 ≤ `k` ≤ `n`
([ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md)). `k` = `n` is the former whole-history
transaction, which the persistence v2 blob profile always uses. Volatile mode (selective vacuum)
copy-builds replacements for selected sparse sealed Segments and publishes them only when the Worker
uses fewer physical Segments afterward.

Only one public compaction attempt may run at a time. Calls are not queued: a concurrent attempt
returns `sequence_conflict`. Mutations never invoke compaction implicitly, and Store shutdown waits
only for the one maintenance transaction already admitted.

A successful result reports either one completed transaction and its copy statistics, or no current
physical gain: with no eligible or physically beneficial work, it returns `compacted == false`.
Durable no-gain planning still fills `worker_index` and the verified sealed Record and byte counters
from the exact Index scan that rejected the rewrite; it never publishes an intent or rewrites
Segments.

For a useful durable transaction, the result also reports:

| Field | Meaning |
| --- | --- |
| `pre_intent_duration_ns` | Scan plus private staged copy, seal and verification before the global Manifest lease |
| `publication_lease_duration_ns` | The recovery-sensitive v1 transaction window |
| `pacing_delay_ns`, `pacing_sleep_count`, `pacing_burst_bytes` | Automatic normal-pressure private-output pacing; manual `compact()` leaves them zero |
| `transient_metadata_lower_bound_bytes` | Fixed snapshot and placement storage, replacement Index allocation and reusable Record scratch; explicitly a lower bound, not total RSS or allocator traffic |
| `source_segments`, `retained_sealed_segments` | The prefix length `k`, and the newer sealed Segments left outside the prefix |
| `remapped_records` | Index entries retargeted to their copy at commit |
| `superseded_copies` | Copies that a concurrent mutation had already superseded; they remain as dead bytes |
| `manifest_rebases` | 1 when the intent was rebased onto a newer Manifest authority |

ADR 0044 added the last four rows. It also retired the concurrent-mutation journal:
`delta_replay_duration_ns`, `delta_records` and `delta_key_bytes` remain for source compatibility
and are always zero.

Compaction preserves logical key/value visibility and may change Record references and physical
Segment identities. Volatile source bytes remain alive while an already returned internal snapshot
retains shared ownership; durable compaction additionally preserves crash recovery authority.

### Automatic maintenance

`StoreConfig::maintenance` and [ADR 0023](../adr/0023-maintenance-controller.md) govern optional
automatic scheduling, described in the
[maintenance controller](../architecture/maintenance-controller.md). The embedded default is
`cooperative`, with no maintenance thread. `background` starts one Store-owned evaluation thread
that may call `compact()` under normal, pressure and emergency budgets; `glifistored` defaults to
`background`. `disabled` starts no thread either and schedules nothing; explicit `compact()` remains
available.

Under emergency, the embedded Store's `put` and `erase` return `storage_exhausted` until capacity
recovers. On the TCP path the Reactor maps that to `OVERLOADED`, and official clients report
`retryability=never`.

The controller settings:

- `dead_byte_ratio_bp_normal`: the normal controller compares the observed dead-byte ratio with this
  threshold.
- `max_copy_bytes_per_cycle`: inclusive limit on the live bytes one normal background compaction of
  a durable Worker copies, 128 MiB by default; zero means unlimited. The runtime compacts the oldest
  sealed prefix that fits within it instead of skipping a larger Worker; when the oldest sealed
  Segment alone exceeds it, the attempt reports no gain. Pressure, emergency and manual `compact()`
  do not apply it, and volatile vacuum ignores it.
- `max_copy_bytes_per_sec`: independent of the per-cycle limit. It paces private pre-intent physical
  writes, admits candidates larger than one second of bandwidth, and uses the controller window only
  to separate adjacent jobs. Named daemon profiles start at 64 MiB/s (`embedded`) and 128 MiB/s
  (`production`); generic Store configuration uses zero (unlimited). Pressure and emergency bypass
  pacing.
- `suspend_on_p99_latency_ms`: can additionally defer normal daemon compaction from a lock-free
  durable-mutation latency window. Zero disables it; embedded Stores receive no samples unless their
  internal host supplies them.
- `suspend_on_p99_min_samples`: rejects undersized windows. The armed guard resumes below 80% of the
  threshold.
- `max_latency_deferral_ms`: bounds continuous postponement of normal reclaim; zero leaves pressure
  as the only bound.
- `max_no_gain_attempts`: normal background maintenance memoizes an unchanged exact no-gain
  candidate after this many attempts, one by default. The memo is invalidated when the physical
  candidate changes, and the candidate is retried no later than `max_eval_interval_ms`. Pressure and
  emergency bypass the memo.
- `max_segments_per_cycle`: persistence v1 accepts only 1. This field is not an incremental-copy
  switch.

Unread expired Records remain conservatively live until GET, recovery or compaction validates their
expiration.

### Maintenance snapshot

```cpp
auto maintenance_snapshot() const -> MaintenanceSnapshot;
```

`maintenance_snapshot()` returns a point-in-time copy of controller state and counters:

- `last_observation`: for durable background maintenance, the selected round-robin Worker and its
  sealed, Index-referenced live, dead and dead-ratio Record-byte counters.
- `*_no_gain_source_*`: last and total scan counters, updated by exact no-gain planning decisions
  but not by cheap policy skips.
- `no_gain_scans_suppressed` counts the scans the no-gain memo avoided, and `no_gain_retry_after_ns`
  reports its remaining bound.
- `rotation`, for durable Stores: runtime-local attempt, commit and wait counters; post-rotation
  final-Record attempt and commit counters; and last, total and maximum nanoseconds for publication
  wait, Segment seal, replacement Segment creation, Manifest publication, aggregate execution, the
  complete rotation and the final Record commit. Publication wait includes acquiring the Manifest
  serializer and any wait for an active compaction lease. Aggregate execution also includes
  planning, reader setup and the in-memory catalog transition; subtracting the three named execution
  phases yields that residual.

The snapshot also exposes the most recently consumed foreground sample count, its fixed-bucket
conservative p99 and the cumulative number of latency-driven suspensions.

## Verification

```cpp
auto verify_index() const -> Status;
```

`verify_index()` checks internal Index and reference invariants. It may stop all Worker progress
briefly by taking Worker locks in a global order. It is intended for tests, diagnostics and
controlled verification, not a high-frequency request path.

Success is evidence that the checked in-memory invariants hold at one synchronized point; it is not
a full disk scrub or a hardware-integrity guarantee.

## Close

```cpp
auto close() -> Status;
~Store();
```

`close()` linearizes when it changes admission from open to closing. Operations admitted before that
point complete; later reads, mutations, verification and flush calls fail with `unavailable`. Close
forces partial strict groups, waits for admitted calls, drains paired Writer lanes when present,
performs the required final durable flush, stops background work and joins internal executors, and
releases Store resources and the data-directory lock.

`close()` is idempotent and returns the sticky result of the first close sequence; concurrent close
callers receive the same outcome.

Destruction performs the same close as a non-throwing fallback but cannot report its result.
Applications that require durable-close error handling must call `close()` explicitly before
destruction. Destruction concurrent with calls that do not otherwise own the `Store` is invalid C++
lifetime usage and is not made safe by internal admission.

## Errors and mutation outcomes

Public errors have a typed category and may carry diagnostic text. No failure category requires
parsing an error message: the text is diagnostic, not stable and not intended for machine parsing.
The current `ErrorCode` set:

| Category | Typical meaning |
| --- | --- |
| `invalid_argument` | Invalid options or operation parameters |
| `arithmetic_overflow` | Checked size, sequence or generation overflow |
| `record_too_large` | Encoded Record exceeds the v1 bound |
| `segment_full`, `segment_sealed` | Append cannot proceed in that Segment state |
| `invalid_record`, `checksum_mismatch`, `corrupted_data` | Malformed or corrupted encoded state |
| `invalid_reference` | Record reference does not identify a valid current extent |
| `sequence_conflict` | Conflicting sequence or mutually exclusive operation |
| `not_found` | Key is absent, tombstoned or expired, or a requested internal object is absent |
| `resource_exhausted`, `storage_exhausted` | Memory/policy or storage capacity exhausted |
| `file_too_large`, `descriptor_exhausted`, `read_only_filesystem` | Stable filesystem/resource category |
| `unavailable` | Store closed, poisoned or temporarily unavailable |
| `io_error` | Operating-system I/O failure not covered more specifically |
| `internal_error` | Invariant or unexpected internal failure |

There is not yet a dedicated `incompatible_format` enumerator: unsupported required persistent
versions surface through an existing validation or corruption category until the planned additive
error-model work lands. Adding a stable compatibility-specific category is release work and must be
additive.

Mutation outcomes follow these rules:

- Invalid input and not-found failures do not mutate state.
- A durable-sync failure before the durable commit point must not reappear after recovery.
- After the durable commit point, failure to publish coherent runtime state is fatal to that Store
  instance rather than a recoverable per-operation error.
- Loss of a network response after commit is an indeterminate client outcome, not evidence that the
  mutation failed.

Exceptions do not cross the supported API boundary for expected operational failures. Allocation and
unexpected implementation exceptions are translated to a stable error or place the Store in a
fail-closed state, as the commit status requires. Deterministic allocator-interposition tests
enumerate the actual allocation sites emitted by each native STL build instead of assuming a fixed,
implementation-specific allocation count.

## Concurrency

Unless stated otherwise, member operations are safe to call concurrently on the same open Store.
This provides per-key operation safety, not transactions across calls or keys: there is no atomicity
across multiple keys in the current API. The API does not promise lock freedom. A completed mutation
happens before a later operation that observes its result.

The default open mode is paired ([ADR 0032](../adr/0032-paired-concurrency-embedded-store.md)):
ordinary GET adopts a published immutable generation, and same-shard `put` and `erase` serialize on
the Writer lane. `StoreConfig::concurrency = StoreConcurrencyMode::legacy_mutex` restores
Worker-mutex serialization; it is deprecated in 0.1.x and removed in 0.2. Mixing legacy and paired
mutators on one Store is refused at open.

Diagnostics return owning snapshots rather than references to live Index, Worker or Segment
containers. Public callers cannot acquire engine mutexes, mutate Segment bytes, publish `RecordRef`
values or invoke unchecked owner-specific paths.

## Compatibility

Version 0.1.x is pre-1.0, and the
[version lifecycle](../architecture/version-lifecycle.md#public-apis-and-abis) sets its API policy.
Source compatibility may change only in a minor release: before 1.0, a minor release may make
intentional source-breaking changes, announced in the changelog and migration notes, while patch
releases within one minor line preserve documented source behavior and do not deliberately break a
supported disk or wire reader. No C++ binary ABI stability is promised before 1.0; consumers rebuild
against each release.

The separately versioned C ABI v1, persistence v1 and wire v2 each have their own compatibility
contract and do not follow the C++ package version automatically: disk, Manifest and wire
compatibility are governed by their encoded versions, not by the library version alone. A release
must not claim alpha status until golden fixtures and a compatibility matrix demonstrate every
supported reader/writer pair. The
[format compatibility matrix](../architecture/format-compatibility.md) lists current development
coverage.

Operator-facing upgrade, downgrade, Worker-count and release-artifact rules are normative in
[version lifecycle](../architecture/version-lifecycle.md)
([ADR 0024](../adr/0024-offline-worker-migration.md)). Worker-count changes use offline
[store migration](../architecture/store-migration.md); reopen never reshards.

Public headers must document ownership, units, limits and thread safety for every new type or
method.

## Prototype migration

The migration from the prototype surface:

1. [x] Introduce owning public key/value and diagnostic types.
2. [x] Replace public `RecordView` reads with the owning result.
3. [ ] Add the pinned handle only together with reader accounting and reclamation tests.
4. [x] Remove `Worker`, `Index`, `Segment`, `GlobalSegmentManager`, `HashedKey` and mutable byte
   access from the supported Store interface.
5. [x] Move corruption and owner-checked hooks into a build-tree-only internal bridge.
6. [x] Export and install only the supported header set.
7. [x] Update the external consumer test to use only the installed owning API.
8. [x] Make ordinary reads and durable recovery use one Store-owned injectable clock.

Temporary compatibility adapters may exist during development, but they must be visibly marked as
deprecated prototype surface and must not survive the alpha release boundary.
