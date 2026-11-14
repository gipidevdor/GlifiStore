# Durability and recovery

**Normative** · persistence v1, embedded Store

The persistence v1 engine contract that the embedded Store implements: storage modes, mutation
ordering and acknowledgement, Segment and Manifest publication, recovery and failure
classification. *Must*, *must not* and *may* state required persistence v1 behavior, not achieved
release status.

GlifiStore is an architectural prototype: process-termination evidence exists, but no filesystem or
device row is E3/E4 certified. Release criteria are in the
[persistence v1 production roadmap](../v1-production-roadmap.md). The exact restart outcome of every
durable intermediate state is in the
[recovery state-transition matrix v1](../spec/recovery-state-matrix-v1.md).

## Storage modes

Creating a `Store` selects exactly one storage mode:

- volatile: Segments and the Index live in memory. A successful mutation means the new state is
  visible to later operations in the same process. Nothing is promised after process exit.
- durable-sync: a data directory is mandatory. A successful mutation means the mutation crossed the
  durable commit point and must be recovered after a process or machine restart, subject to the
  documented filesystem and hardware guarantees.
- durable-periodic: a data directory is mandatory. A successful mutation means the mutation was
  published in memory and is visible to later operations in the same process. It is not promised
  after restart until a background or explicit flush has synchronized its Segment commit slot. The
  maximum loss window is `sync_interval_ms` plus any in-flight unflushed writes
  ([ADR 0010](../adr/0010-durable-periodic-policy.md)).
- durable-group: a data directory is mandatory. A successful mutation means the batch containing it
  completed commit-slot publication and platform synchronization. Clients may block until the batch
  closes. There is no durability loss window ([ADR 0011](../adr/0011-durable-group-commit.md)).

Group commit batching may also be enabled for durable-periodic, to reduce per-Record commit-slot
writes while preserving the periodic visibility contract of ADR 0010.

## Persistent identities and versions

Disk, Manifest, Record and wire versions are independent. The Manifest records at least:

- the Store identity generated at creation;
- the Manifest format version;
- the Segment and Record format versions;
- the routing algorithm identifier and seed (FNV-1a by default; persisted SipHash-2-4 for keyed
  Stores);
- the persisted Worker count and routing epoch;
- the complete Segment catalog and the active Segment of every Worker;
- the next Segment ID and the generation information needed to prevent stale references.

Worker auto-sizing runs only when a durable Store is created. Reopen uses the persisted Worker
count, even when the machine topology has changed. Supplying an incompatible Worker count, routing
algorithm or routing epoch must fail before any file is mutated. Online resizing and routing
migration are outside the 0.1.x scope.

Unknown required format versions fail closed. Optional forward-compatible fields must be
length-delimited and safely skippable, and no decoder may allocate directly from an unchecked
persisted size. A correctly checksummed unknown commit-slot version is an incompatibility; it cannot
be treated as a damaged slot eligible for fallback. The exact implemented layouts are in
[Segment and Record format](segment-format.md) and in [Manifest format](manifest-format.md), which
also states the catalog invariants.

## Data directory

A durable Store holds one exclusive process lock on its data directory. The directory contains a
single authoritative Manifest and a Segment namespace. Temporary publication files and orphaned
Segments are distinguishable from committed files by name and Manifest membership.

The implementation must not use `/tmp` as a default, and must reject symlinks and other filesystem
objects that violate the documented data-directory policy. Filenames are implementation details:
Store identity, Segment identity, generation and ownership come from validated encoded metadata,
not from parsing a filename alone.

Unknown files are reported, and quarantined or rejected according to an explicit operator policy;
they are never silently added to the Store. A Manifest-listed file that is absent, duplicated or
inconsistent is corruption and prevents read-write open.

## Resource preflight

The durable resource policy is supplied at open and is deliberately absent from v1 metadata. Reopen
first limits the Manifest bytes before allocating their decode buffer, then checks Segment count,
steady logical Store bytes, descriptor policy and `RLIMIT_NOFILE`, recovery memory and
Worker-partitioned live-key capacity. A smaller policy fails read-only with a stable resource
category.

Creation computes its peak as all initial 64 MiB Segments plus the bootstrap intent and Manifest
that are present at the same time. Rotation computes the replacement Segment plus the current and
replacement Manifest generations. The preflight samples the space available to the unprivileged
process and preserves `reserved_free_bytes`; a failure occurs before intent publication or
active-Segment sealing. The sample cannot reserve space against unrelated processes, so native
full-file preallocation remains mandatory and its `ENOSPC` or quota result is authoritative.

Recovery scans one Worker at a time and charges a conservative estimate for catalog arrays, Worker
state, temporary latest-key nodes, duplicated key bytes and the resulting Index. It returns
`resource_exhausted` before exceeding the configured estimate. The estimate is a safety ceiling, not
allocator-exact telemetry.

## Segment header and commit slots

Every Segment is exactly 64 MiB. Its first 4 KiB hold an explicitly little-endian header with
immutable identity fields and two independently checksummed commit slots. No field is decoded
through C++ object layout.

The immutable header fields include at least the Segment magic and format version, Store identity,
Segment ID, generation, owner Worker, Record format version and header size. Each commit slot holds
at least:

- slot format version and CRC32C;
- monotonic commit generation;
- committed end offset;
- committed Record count;
- first and last committed sequence where present;
- the lifecycle state required for active and sealed recovery.

The committed end starts at byte 4096, is 8-byte aligned, never exceeds Segment capacity and never
moves backwards. Writers alternate slots and never overwrite the only known-valid newest slot.
Recovery validates both slots and selects the valid slot with the greatest commit generation. Two
different valid slots with the same generation are corruption unless their complete decoded
contents are identical.

Bytes after the selected committed end have no logical meaning and are never scanned. Corruption, a
checksum failure or an invalid Record inside the committed extent is fatal corruption, never a crash
tail. Commit slots exist to make this distinction.

## Mutation ordering and acknowledgement

Volatile mode keeps the append-then-publish behavior, but must still fail closed if a rollback
cannot restore a coherent Index and liveness state.

### Durable-sync

Durable-sync mutation processing follows this order:

1. Validate arguments, limits, ownership, sequence availability, and all fallible publication
   capacity needed by the in-memory Index.
2. Encode the complete immutable Record outside the committed extent.
3. Write all Record bytes and establish the platform Record-before-slot ordering boundary.
4. Encode the alternate commit slot with the new extent and sequence, write it, and synchronize the
   Segment again.
5. Treat successful synchronization of that commit slot as the durable commit point.
6. Publish the `RecordRef`, liveness changes, and visibility in memory.
7. Return success or encode the successful network response.

A failure before step 5 returns an error and must not become visible after recovery. Once step 5
has succeeded, a publication failure is a fail-closed engine event, because recovery will observe
the mutation; the process must not keep serving a state that disagrees with its durable log.

An acknowledged mutation must survive restart. A client that loses its connection after the commit
point but before receiving the response has an indeterminate outcome, and must read the key before
deciding whether to retry. Idempotency keys and cross-key transactions are not part of the current
contract.

### Durable-periodic

Durable-periodic mutation processing follows this order:

1. Validate arguments, limits, ownership, sequence availability, and all fallible publication
   capacity needed by the in-memory Index.
2. Encode the complete immutable Record outside the committed extent.
3. Write all Record bytes without synchronizing the Segment data or publishing a commit slot.
4. Publish the `RecordRef`, liveness changes, and visibility in memory.
5. Return success or encode the successful network response.
6. When the batch closes, establish the platform Record-before-slot ordering boundary, publish one
   alternate commit slot, and synchronize that slot. `Store::flush()` and orderly shutdown force the
   same sequence.

A failure before step 4 returns an error and must not become visible in the running process. A
successful response does not imply restart durability until step 6 completes. An unflushed
mutation may or may not survive a crash, but Record-before-slot ordering ensures that a surviving
valid slot never authorizes an unsynchronized Record extent.

When batching is enabled for durable-periodic, step 3 writes Record bytes without publishing a
commit slot, and steps 4 and 5 occur immediately after each Record write. A foreground threshold or
a background deadline then performs step 6 for the accumulated extent; `Store::flush()` and orderly
shutdown force it synchronously.

`Store::flush()` and `Store::close()` synchronize all dirty Segment files before releasing the
data-directory lock. Close returns the sticky final-flush result; destruction takes the same path
but cannot expose its status. A flush failure is fail-closed.

### Durable-group

Durable-group mutation processing follows this order:

1. Validate arguments, limits, ownership, sequence availability, and publication capacity.
2. Encode the complete immutable Record outside the committed extent.
3. Write all Record bytes without publishing a commit slot.
4. When the Worker batch closes by its adaptive record target, byte threshold, or `max_wait_ms`,
   establish the platform Record-before-slot ordering boundary, publish one commit slot, and
   synchronize the Segment again. The record target remains within the configured `min_records`
   and `max_records` bounds.
5. Publish the `RecordRef`, liveness changes, and visibility in memory for each mutation in the
   closed batch while the commit executor owns the Worker lock.
6. Return success or encode the successful network response.

A failure before step 4 returns an error and must not become visible after recovery. Once step 4 has
succeeded for a batch, every mutation in that batch must survive restart. The commit executor
completes all in-memory publication before waking the batch waiters; a flush or publication failure
wakes all waiters with the runtime fail-closed. A full Segment first closes any staged batch, then
rotates.

The one-Worker runtime delegates the commit phases to its durability coordinator and bounds
admission when the adaptive record target or `max_bytes` closes the batch. Deadline-limited
occupancy contracts the next target and already-admitted producer pressure grows it, without
changing the absolute deadline or the acknowledgement semantics. A multi-Worker Store keeps
independent Worker-local producer-closed batches and commit domains.

The paired Writer may reach one or more record or byte thresholds while appending a caller batch. In
strict mode every threshold closes with the ordered Record barrier, the commit-slot write, the final
slot synchronization and Index publication. `commit_writer_batch()` performs the same immediate
sequence for the residual extent after the append loop. Publishing a threshold-closing slot as
deferred, and then treating an empty explicit commit as proof of strict durability, is invalid.

## Segment creation, rotation and Manifest publication

Before the first committed Record enters a new Segment, durable-sync mode must:

1. create and size a temporary Segment file;
2. write and synchronize its initial header and commit slots;
3. publish its final filename and synchronize the Segment directory;
4. publish a new Manifest using temporary file, file synchronization, atomic rename, and parent
   directory synchronization;
5. only then make the Segment writable by its owner.

Rotation seals and synchronizes the old Segment first, which creates an unambiguous intent marker.
It then creates and synchronizes the exact next-identity replacement and publishes the Manifest that
makes the replacement active. A crash may therefore leave the selected Manifest naming a sealed
active Segment, with or without one pristine prepared replacement. Runtime open validates or creates
only that exact replacement and publishes the completed rotation before serving writes. It never
reopens the sealed Segment for append; any other lifecycle mismatch is corruption.

Runtime rotation copies the old Segment identity before replacing the in-memory Manifest, so cache
retirement never keeps an iterator into the replaced catalog.

A created Segment absent from the selected Manifest is an orphan, not part of the Store. Engine
temporary files and otherwise valid orphan Segments may be quarantined without adoption; malformed
or unexpected objects follow the configured reject or quarantine policy. Recovery must advance or
safely reserve Segment IDs, so that an orphan can never collide with a newly created Segment.

Manifest generations are monotonic, and publication never modifies the currently selected Manifest
in place. If the platform cannot provide the required atomic replacement and directory
synchronization semantics, durable-sync open must fail as unsupported.

## Recovery

Recovery runs before network listeners start and before the Store accepts operations:

1. acquire the exclusive data-directory lock;
2. dispatch on a durable intent in the order of the
   [recovery state-transition matrix v1](../spec/recovery-state-matrix-v1.md): complete a bootstrap
   intent, admit exactly the old or next Manifest of a compaction intent, or complete one exact
   interrupted rotation;
3. select and validate the newest complete Manifest publication;
4. validate Store identity, versions, routing metadata, and catalog invariants;
5. audit the data-directory namespace against that Manifest and reject blocking entries before
   opening any Segment;
6. open every Manifest-listed Segment and validate its identity against the catalog;
7. select the newest valid commit slot for each Segment;
8. scan exactly each committed extent and validate every Record checksum and bound;
9. require every non-empty Segment's first sequence to exceed the preceding committed range for
   that Worker;
10. rebuild visibility by full key and highest sequence, with tombstones and expiration suppressing
    older values;
11. partition rebuilt entries using the persisted routing configuration;
12. restore each Worker's next sequence using checked `maximum + 1` arithmetic;
13. only then remove non-authoritative names, namely recognized engine temporaries or the obsolete
    names and the intent of a compaction transaction, and synchronize the directory;
14. audit the namespace again and validate the complete rebuilt state before publishing the Store
    as ready.

Filesystem enumeration order must not affect the result; canonical Manifest order is authoritative.
Equal, overlapping or reversed per-Worker Segment ranges fail recovery. Equal highest sequences for
the same key are a conflict. Sequence exhaustion fails read-write open. A future read-only salvage
mode must be a separate, explicit operator action, and must never silently weaken normal open.

Expiration uses absolute Unix-epoch nanoseconds. Recovery keeps the newest Record decision even when
that Record has already expired; it must not resurrect an older value. Clock rollback behavior and
test clock injection belong to the public time contract.

## Failure classification

- An uncommitted trailing write is ignored, because it lies beyond the selected committed extent.
- A malformed or checksummed Record inside a committed extent is corruption.
- A missing or inconsistent Manifest-listed Segment is corruption.
- An unknown required version is incompatibility, not generic corruption.
- Disk-full and I/O failures before the commit point fail the operation without acknowledgement.
- An I/O or publication failure after the commit point makes the runtime unhealthy and stops service
  until recovery.

Errors must preserve these categories for CLI diagnostics and automation. Recovery must never
repair, truncate, delete or rewrite source files during an ordinary read-write open. Repair and
salvage tools operate on a copy, or require an explicit destructive flag added under a separate
contract.

## Implementation

| Layer | Implements |
| --- | --- |
| [Persistence filesystem layer](persistence-filesystem.md) | low-level ordering, platform flush choices, namespace protections and publication outcomes |
| [Durable Segment files](segment-filesystem.md) | exact-size creation, alternate-slot commit and seal, bounded recovery scan |
| [Durable recovery implementation](recovery-implementation.md) | Manifest-driven Segment validation, partitioned Index rebuild, sequence restoration, interrupted-rotation detection |
| [Data-directory namespace audit](namespace-policy.md) | descriptor-relative namespace audit and strict normal-recovery policy |
| [Durable runtime catalog](durable-runtime-catalog.md) | bounded verified reads, durable mutation ordering, exact-intent rotation completion; owned by the public Store PImpl |

Offline inspection and fail-closed repair tools exist. Live or hot backup and in-place destructive
rewrite stay out of the ordinary open path.

## Required evidence

Persistence v1 release evidence requires CI coverage of:

- golden fixtures for the Manifest, Segment header, commit slots and Records;
- restart tests across every mutation and rotation transition;
- process kill and injected short read or write, `EINTR`, writeback `EIO`, rename,
  disk/quota/read-only and allocation failures;
- active-tail tolerance and committed-region corruption rejection;
- Manifest rollback and explicit orphan quarantine and identity reservation;
- routing and Worker-count mismatch rejection;
- recovery independence from Segment enumeration order;
- compatibility reads of artifacts emitted by every supported released format version.

Current development coverage is in the [format compatibility matrix](format-compatibility.md).
Cross-release compatibility and pinned filesystem power-loss campaigns are release gates; see the
[platform durability evidence matrix](platform-durability-evidence.md). Codec unit tests alone are
not durability evidence.
