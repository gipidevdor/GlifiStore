# Persistence v1 production roadmap

**Roadmap** · persistence v1, durable runtime, TCP daemon · reviewed 2026-08-01

The ordered backlog and acceptance criteria for persistence v1, the durable runtime, the TCP daemon,
tests, operations, security and release evidence, up to the definition of a production-ready v1.

Persistence stays on the existing v1 Manifest, Segment header, alternating commit slots and Record
format. Storage modes differ only in acknowledgement and batching policy; the native wire protocol
has its own independent version.

## Current status

### Paired runtime (0.1.0)

`glifistored` has a single daemon runtime: Reader–Writer shard pairs (ADR 0031/0032). There is no
dual-select switch and no "legacy until migration" daemon path.

| Surface | Path | Notes |
|---|---|---|
| Public embedded `Store::get` | Owning pin (`OwnedValue`) | ADR 0009; unchanged |
| Daemon GET | Borrowed Reader-local `ReadGeneration` | Adopted once per Reactor turn |
| Mutations | Bounded SPSC → serial Writer per shard pair | `Server::pair_writer_stats()` |
| CLI | `--shard-pairs` canonical; `--workers` alias | Same count as Manifest/wire `worker_count` |
| Lab | ADR 0036 generation laboratory in `src/experimental/` | Not installed; not reachable from `glifistored` |

What remains of the [paired shards plan](benchmarks/paired-shards-plan.md) is performance evidence,
not a second runtime: Delta mixed magnitude on the Linux hard-pinned A/B; get-into/multi-extent,
rejected pending proof; the Linux 1/2/4/8 harness, waiting on `glifistore-linux-perf`; and an
optional Linux I/O backend, deferred without an ordering change.

### Implemented foundation

Little-endian codecs, checksums, immutable Records, exact-key indexing, descriptor-relative storage
access, exclusive Store locking, preallocated fixed-size Segments, atomic Manifest replacement,
bounded namespace auditing, fail-closed read validation, platform pollers, sanitizer jobs, golden
disk fixtures and process-termination tests for v1 write boundaries.

### Open release gates

| Item | Still open |
| --- | --- |
| P0-01, daemon operability (P1) | Operability surfaces beyond the bounded durable lanes, STATS latency histograms and maintenance rate-window needles |
| P0-02 | Long-running native-platform clock evidence |
| P0-03 | Native-platform evidence for the exception and allocation boundaries |
| P0-04 | Released-artifact and native-platform evidence for cross-Segment sequence validation |
| P0-07 | Sudden power loss and filesystem certification (E3/E4, physical E3); process kill is E2 evidence only |
| P0-08 | Controlled native baselines and power-loss certification; the software policy is closed |
| Backup (P1) | Hot zero-fence backup |
| Supply chain (P2) | Full SLSA L3, project GPG |
| Security (P1) | Multi-tenant Phase 8; live OCSP over HTTP is unsupported; configure `--tls-crl` before exposure to the hostile public Internet |

Local throughput gains do not close these gates.

## Non-negotiable v1 invariants

Every implementation block below must preserve these rules:

1. A strict mutation is acknowledged only after its Record bytes and selected v1 commit slot meet
   the documented platform persistence ordering.
2. Recovery uses only committed v1 extents named by a valid v1 Manifest. Uncommitted tails never
   become visible.
3. Per-Worker sequence numbers are globally strict and monotonic across Segment rotation. Equal,
   overlapping, or reversed ranges are corruption.
4. Runtime state is never published before commit. A failure after commit is either represented as
   an explicit indeterminate outcome or makes that Store instance fail closed.
5. Every externally controlled length, count, offset, time, and resource total is checked before
   conversion, addition, allocation, or I/O.
6. Durable recovery never silently edits suspicious state. Verification is read-only; repair is a
   separate, explicit, auditable command.
7. Algorithms must have a documented worst-case resource bound. An optimization lands only with
   correctness tests, representative p50/p95/p99/p99.9 evidence, and no regression in crash tests.
8. Linux, macOS, FreeBSD, and OpenBSD use one logical contract but may use separately proven
   allocation, synchronization, polling, and cache strategies.

## P0 — correctness and durability blockers

### P0-01 — Make the daemon capable of v1 durability

Status: software-complete for the embedded and TCP daemon paths; native power-loss certification
(E3/E4) remains open. Operator guide: [durable TCP daemon](operations/durable-tcp-daemon.md).

- `Server::create` accepts a complete `StoreConfig`, requires the Worker count to match the
  executor count and closes the Store from `join()`. The daemon selects `volatile`,
  `durable-sync`, `durable-periodic` or `durable-group` and exposes durable data-directory and
  open-policy controls.
- Durable `PUT`/`ERASE` leave the Reactor through bounded per-Worker FIFO lanes with count and byte
  admission, generation-safe completion, overload responses, queue-wait expiry before Store entry,
  per-lane metrics ([observability](operations/observability.md)) for latency, Writer collection
  wait and close reason, and sync-to-async fairness turns, and drain-before-Store-close shutdown
  (`--shutdown-drain-ms`, default 30 s). Strict-group mode retains bounded concurrent producers.
- Wire `HEALTH`/`READY`/`STATS` expose liveness, readiness (including maintenance emergency) and a
  bounded ASCII admin report with maintenance rate-window needles.
- The daemon CLI exposes durable batch, resource and maintenance caps (including per-second/CPU rate
  budgets), and `--dump-config` covers the effective durable settings. File/environment
  configuration precedence and the deployment profiles (`dev`, `embedded`, `production`) validate
  fail-closed before listen. Secure-profile authn/authz (`--authz-map`, `--secure-profile`) is
  wired.
- Real-daemon wire-protocol SIGKILL coverage (`glifistore_crash_daemon`) exists for post-ack PUT,
  pre-commit PUT and post-ack ERASE. Integration tests cover emergency-gate wire `OVERLOADED`
  rejection and durable wire ERASE through reopen.

Required change: a validated `StoreConfig` into `Server`; CLI and configuration for data directory,
open policy, durability policy, batch limits and recovery policy. Durable writes must leave the
Reactor thread and complete through a bounded asynchronous path; an `fsync` must not block the event
loop. A success response may be queued only after the selected durability policy permits
acknowledgement.

Acceptance: restart and SIGKILL tests use the real daemon and wire protocol; strict/group success
responses always recover; pre-commit kills do not expose the mutation; disconnect after commit is
documented as indeterminate; slow storage cannot stall unrelated Reactor I/O; invalid or
conflicting configuration fails before listening.

### P0-02 — Implement the documented Store clock and TTL semantics

Status: implemented; native-platform and long-running clock evidence remain a release gate.

Change: ordinary reads and durable recovery share a Store-owned clock. Production defaults to
checked Unix-epoch nanoseconds; deterministic tests inject a thread-safe `StoreClock`. Per-call
public timestamps were removed, and a nonzero `recovery_now_ns` is rejected. Each Store clamps
backward movement with an atomic high-water mark. Monotonic time remains reserved for batching
deadlines, never persisted absolute expiry.

Acceptance: an ordinary `get(key)` expires data without caller assistance; restart prunes the same
logical expirations; tests cover equality, zero/no-expiry, maximum timestamp, backward and forward
clock jumps, and conversion overflow.

### P0-03 — Close every exception and allocation boundary around commit

Status: implemented; native-platform evidence remains a release gate.

Change: public operations translate allocation and unexpected exceptions; background callback
exceptions stop the coordinator, fail-close the runtime and release batch waiters. Default paired
opens disable the duplicate hot cache; on the deprecated `legacy_mutex` path, hot-cache
key/value/node/capacity preparation happens before persistent writes, and post-commit publication
uses prepared node insertion.

Required change: inventory every allocation and throwing operation before and after commit.
Preconstruct all fallible publication state before commit, publish prepared no-throw nodes or remove
the duplicating hot cache. Catch allocation and unexpected exceptions at the public API and thread
entry points. Record whether commit was reached and return a stable resource, internal or
indeterminate category, or atomically fail closed, as appropriate. No queued pointer may outlive its
caller during cancellation, shutdown or exception unwinding.

Acceptance: deterministic allocation-failure injection at every site proves that pre-commit failure
does not recover, post-commit failure never permits continued ambiguous use, all waiters are
released, and no exception terminates a worker or crosses the supported API.

Evidence: `glifistore_allocation_fault_tests` creates a fresh v1 Store for every Nth-allocation
failure, checks the persistent write boundary through filesystem hooks, reopens pre-write and
interrupted-rotation states, verifies sticky fail-close after uncertain outcomes and rejects any
steady-state allocation after `write_record`. An isolated test executable interposes every throwing
`new` form and fails each allocation observed by the native STL build in the put, update, erase,
owning read, strict group commit and Segment rotation paths. A background `bad_alloc` test joins
every strict-group producer and proves that no caller-stack pending pointer survives the failed
batch.

### P0-04 — Enforce cross-Segment sequence ranges

Status: implemented; released-artifact and native-platform evidence remain to accumulate. Recovery
rejects equal, overlapping or reversed non-empty ranges in later Manifest-ordered Segments of the
same Worker before scanning their Records.

Required change: validate the first and last committed sequences of every non-empty Segment against
the preceding Segment in Manifest order. Define empty active Segment behavior explicitly.

Acceptance: fixtures and corruption tests reject duplicate, overlapping, reversed, exhausted and
inconsistent ranges, including equal sequences attached to different keys. Valid rotations and an
empty new active Segment continue to recover.

### P0-05 — Make background flush failure and shutdown observable

Status: implemented. Coordinator callback exceptions map to sticky `resource_exhausted` or
`internal_error` failures, stop the coordinator, fail-close the durable runtime and release queued
batch waiters. Public `Store::close()` is an explicit, idempotent shutdown that returns a sticky
status; the destructor runs the same close as a non-throwing fallback and discards its status, so
it is not the only durability barrier. The [C++ API reference](reference/cpp-api.md) specifies the
close sequence.

Acceptance: injected callback exceptions, I/O errors, stop races, maximum generation and concurrent
`flush()`/close tests cannot deadlock or acknowledge unflushed state.

Evidence: tests cover repeated and concurrent close, concurrent flush/close, partial strict groups
with a 60-second normal deadline, immediate reopen while the closed Store object remains alive,
sticky final-sync failure, background callback exceptions, coordinator stop races and exhausted
flush generations.

### P0-06 — Add storage and recovery resource budgets

Status: implemented for the embedded v1 Store. `DurableResourceLimits` bounds peak Store bytes, the
available-space reserve, Segment count, Manifest bytes, Store descriptors, estimated recovery
memory, live keys, temporary compaction space and write amplification. These are runtime policy,
described in the [C++ API reference](reference/cpp-api.md), and do not change the v1 disk format.

Required change: validated limits for each of these resources; bootstrap and rotation preflighted
before partial state is published; `ENOSPC`, `EDQUOT`, `EFBIG`, `EMFILE` and memory exhaustion
mapped to specific stable errors.

Acceptance: boundary tests for every quota fail deterministically and leave a recoverable Store;
opening 256 Workers cannot reserve space without an explicit sufficient budget; recovery reports
resource exhaustion rather than terminating.

Evidence: bootstrap accounts for the simultaneous intent, Manifest and active Segments before
publishing the intent. Rotation accounts for the replacement Segment and both Manifest generations
before sealing the old active Segment. Reopen bounds Manifest allocation before decode, validates
steady Store bytes and descriptor/RLIMIT requirements, and applies a conservative key-aware
recovery-memory estimator. Tests cover invalid fields, configured boundaries, injected available
space, 256-Worker rejection with no bootstrap intent, reusable live-key capacity, constrained
recovery, and rotation rejection while the persisted active Segment remains unsealed. Native
`ENOSPC`/`EDQUOT`, `EFBIG` and `EMFILE`/`ENFILE` map to `storage_exhausted`, `file_too_large` and
`descriptor_exhausted`.

### P0-07 — Complete the failure matrix beyond process termination

Status: deterministic in-process coverage and the evidence-collection foundation are implemented;
hardware and filesystem certification is open. No row is E3/E4 certified.

- Process-kill tests cover the v1 bootstrap, put, periodic/group put and rotation checkpoints.
- Instance-local raw I/O seams force short `pread`/`pwrite`, `EINTR`, synchronization `EIO`,
  `ENOSPC`/`EDQUOT` and `EROFS`.
- The [platform durability evidence matrix](architecture/platform-durability-evidence.md) defines
  cumulative E0–E4 claims, provenance and promotion rules, and a collector for native E2 artifacts.
- The [E3 campaign](operations/e3-campaign.md) path rehearses linux-ext4 loopback and macOS APFS
  diskimage resets through the block-reset harness (`scripts/run-e3-block-reset.sh`), the campaign
  orchestrator (`scripts/run-e3-campaign.sh`), the honesty assert
  (`scripts/assert-e3-rehearsal-honesty.sh`) and the weekly `durability-evidence.yml` jobs. It stays
  `e3_certified=no`.
- Controller caches, torn sectors, sudden physical power loss and reviewed pinned native filesystem
  rows still require dedicated lab campaigns beyond hosted CI; reviewed E3 campaigns on pinned lab
  disks and E4 release artifacts remain open.

Required change: deterministic short-write/read, `EINTR`, delayed writeback `EIO`, `ENOSPC`,
`EDQUOT`, `EROFS`, missing file, corrupt directory entry, rename, file-sync and directory-sync
faults; VM/block-device power-cut tests and a documented filesystem/mount matrix.

Acceptance: every persistent transition has a pre/post failure oracle; two crash matrices can run
concurrently; supported filesystem rows pass repeated power-cut recovery; unsupported remote or
user-space filesystems are rejected or prominently documented.

Evidence: exact-I/O tests prove retry after `EINTR`, completion after repeated short transfers, and
stable native error categories. Manifest, bootstrap intent and Segment creation matrices verify
every pre-rename boundary. A directory-sync failure after rename is indeterminate and fail-closed.
Durable mutation tests cross write, Record sync, commit-slot write and commit-slot sync with
`io_error`, `storage_exhausted` and `read_only_filesystem`, then reopen and verify the recovery
oracle. Namespace recovery rejects missing catalog files, malformed names, symlinks, hard links and
unlisted entries without adopting them. NFS, SMB, FUSE, overlay and other remote or user-space
storage are unsupported.

### P0-08 — Implement crash-safe durable compaction in v1

Status: partial. Controlled native benchmark baselines and native power-loss certification remain
open.

- Durable compaction is cooperative `Store::compact()` maintenance behind a non-queuing Store-wide
  maintenance gate, with an optional Store-owned
  [MaintenanceController](architecture/maintenance-controller.md) (ADR 0023). Its automatic reclaim
  policy (budgets, pressure, emergency mutation rejection and lifecycle fail-closed) is implemented:
  an inclusive normal threshold over exact per-Worker sealed/live/dead byte counters, a finite
  128 MiB default live-byte preflight, daemon-configurable per-candidate copy limits and
  per-second/CPU rate budgets, and a no-gain memo that replaces monolithic rescans.
- Durable mutation lanes reject before enqueue while the maintenance emergency gate is armed, and
  official clients map wire `OVERLOADED` to `retryability=never`.
- A deterministic v1 planner replaces the oldest sealed prefix of one Worker: the first `k` of its
  `n` sealed Segments in Manifest order (`1 ≤ k ≤ n`), where `k = n` is the former whole-history
  transaction ([ADR 0044](adr/0044-bounded-prefix-compaction-remap.md)). Descriptor-relative intent
  publication, restart resolution against exactly the old or next authority, and online single-
  and multi-output crash/I/O fault matrices are implemented.
- [ADR 0039](adr/0039-pre-intent-staged-compaction-output.md) completes output copy, seal and
  verification before the intent and the publication lease, so Segment-sized copy I/O does not
  block unrelated rotations; rotation waits on the lease instead of rejecting unrelated Workers.
  Recovery cleans recognized pre-intent temporaries after the authoritative scan.
- Compact placement metadata does not duplicate owning `IndexEntry` objects. Public diagnostics
  separate pre-intent time, lease time and a conservative transient-metadata lower bound.

Required change: copy only the latest live v1 Records into new v1 Segments, validate the copy,
atomically publish a new v1 Manifest, sync the directory, then retire old files with a second
directory sync. Define pin/reader ownership, tombstone and TTL retention, the temporary-space
budget, recovery of recognizable compaction temporaries and an online scheduling policy.

Acceptance: kills at every copy, validation, Manifest, rename, unlink and directory-sync boundary
recover exactly one valid catalog; readers never observe deleted backing files; space and write
amplification remain within configured limits; no format change is introduced.

Evidence: planner, intent codec, filesystem, integration recovery, builder, runtime, model-history,
SIGKILL and public integration coverage are summarized in
[crash-safe durable compaction](architecture/durable-compaction.md) and the crash harness profiles
(`standard`, `copy-matrix`, `random-matrix`, `random-campaign`).

## P1 — complete the product contract

### API, error model, and lifecycle

- Add stable categories for resource exhaustion, incompatible format, permission denial, lock
  conflict, indeterminate mutation, cancellation and internal fail-closed state instead of
  overloading `invalid_argument`, `unavailable` and `io_error`.
- State the `Result::value()` precondition or provide non-throwing accessors. Keep diagnostic text
  explicitly non-stable and expose structured fields where automation needs them.
- Add cancellation/deadline semantics, immutable diagnostic snapshots, build/version information
  and recovery statistics. Do not advertise `PinnedValue` until its reader accounting and
  reclamation design is implemented and tested.
- Extend the implemented concurrent close/flush/read/mutation linearization with model/property
  histories, diagnostic lifecycle rules and multi-Worker group completion fairness.

### Recovery scalability and read-path bounds

- Replace the implemented conservative recovery-memory estimator with allocator-accounted arenas or
  bounded sorted spill runs if measurements show unacceptable rejection or scale. A checkpointed
  derived index remains gated on crash and compatibility proofs.
- Parallelize recovery by Worker only behind descriptor, memory and I/O-token limits. Compare
  sequential and bounded parallel scans on SSD, HDD and constrained containers.
- Replace temporary allocating string lookups with transparent `string_view` hashing/equality.
- Cold reads acquire an immutable exact-generation handle pin with their `RecordRef`, perform all
  positional I/O and validation without the Worker or catalog lock, and linearize at a final locked
  `Index`/pin identity check. Tests suspend `pread` while a same-Worker mutation completes and while
  compaction publishes and retires the source. Descriptor use is bounded by catalog Segments plus
  mutable Worker handles. Default paired opens disable `hot_records` and serve ordinary GETs from
  immutable generations ([ADR 0032](adr/0032-paired-concurrency-embedded-store.md)); only the
  deprecated `legacy_mutex` path, scheduled for removal in 0.2, still admits hot entries.
- Validated GET expiry lazily removes the Index entry (`erase_no_compact`) and, where the deprecated
  `legacy_mutex` hot cache is enabled, the matching hot-cache row, freeing live-key budget without a
  durable tombstone; repeated GETs of a reclaimed expired key are Index misses. Sealed durable
  compaction drops Index-resident expired puts (`expired_records_dropped` on `CompactionResult` /
  `MaintenanceSnapshot`); active-Segment expired Index entries remain until GET reclaim or recovery.
  Physical TTL cleanup of sealed history is therefore measured: isolated local benefit/cost and
  concurrent-maintenance diagnostic runs cover TTL and useful/no-gain shapes, overwrite-driven
  dead-byte selection and foreground tail cost under publication conflict. Normal unread-TTL
  scheduling is opt-in and fail-closed by default (`unread_ttl_normal_scheduling`); the
  pressure/emergency probe and telemetry are closed. Useful-compaction copy pacing is implemented
  ([ADR 0040](adr/0040-pre-intent-compaction-write-pacing.md),
  `--maintenance-max-copy-bytes-per-sec`) with local macOS evidence only
  ([compaction pacing record](../benchmarks/results/local-macos-2026-08-27-compaction-pacing/README.md)).
  Remaining open work is controlled native baselines, including native pacing campaigns, and native
  power-loss evidence under P0-08.

### Offline verification, backup, restore, and repair

- The offline tools exist and fail closed ([CLI reference](cli.md)): `glifistore_inspect_segment`
  is a v1-aware, bounded, read-only Segment validator; `glifistore_verify_store` validates the
  Manifest, the namespace and every catalog Segment under an exclusive data-directory lock;
  `glifistore_backup_store` makes offline verified backup and restore copies; and
  `glifistore_repair_store` repairs into an explicit empty workspace and never mutates the source.
  In-place destructive rewrite remains forbidden. `glifistore_rebuild_index` permanently refuses
  offline Index rewrite for durable v1, with an explicit recovery/repair operator path; durable
  Indexes are rebuilt by Store recovery.
- Online fenced backup is available through `Store::backup_to`, live-daemon wire `BACKUP` (opcode
  10, admin-gated), typed C++ `Client::backup` and the official SDK `backup` helpers. Admission
  pauses during the flush, structural source check and catalog copy; destination CRCs are verified
  after resume; Segments are copied in bounded parallel. It is not fully hot concurrent I/O. CI
  greps the typed surface with `scripts/assert-sdk-backup-helpers.sh` and runs a runtime smoke with
  `scripts/test-sdk-backup-interop.sh`. Live (hot) backup with zero writer fencing remains open.
  The online fenced backup and the offline contract are in
  [backup and restore v1](spec/backup-restore-v1.md); operator procedures are in the
  [backup-restore runbook](operations/backup-restore.md) and the
  [corruption-repair runbook](operations/corruption-repair.md).
- Publish upgrade/downgrade rules for persistence v1 and test artifacts created by every released
  reader/writer. Worker-count changes and resharding require an offline, resumable, verified v1
  migration tool rather than an implicit open-time rewrite; an offline migrate tool exists.

### Daemon operability and protocol

- Add bounded durable completion queues, overload responses, per-request deadlines and fairness so
  that one Worker or slow client cannot monopolize an executor. Implemented under P0-01: bounded
  per-Worker mutation lanes with count and byte admission (`--durable-mutation-queue-capacity`,
  `--durable-mutation-queue-bytes`), wire `OVERLOADED`, a pre-Store queue-wait deadline
  (`--durable-mutation-queue-wait-ms`), `--request-timeout-ms` and sync-to-async fairness turns.
  Multi-hour adversarial fairness evidence remains open.
- Define graceful drain: stop accepting, bound the drain deadline, finish or classify committed
  writes, flush and close the Store, and only then exit. Test SIGINT/SIGTERM during every phase.
  Implemented: `SIGINT` and `SIGTERM` start that sequence, bounded by `--shutdown-drain-ms`
  ([graceful drain and overload](operations/graceful-drain-and-overload.md)). Integration tests
  drive shutdown in process through each drain phase, and the runbook smoke (`ops-runbooks.yml`)
  and soak runs stop the real daemon with `SIGTERM`; a signal sent during each phase is not yet
  tested.
- Add liveness, readiness, structured logs, metrics, a build/config dump and an administrative
  diagnostic surface. Readiness must fail on sticky storage errors and during unsafe recovery.
  Implemented: wire `HEALTH`/`READY`/`STATS` and `glifistored --dump-config`, with durable lane
  latency histograms and maintenance rate-window needles
  ([observability](operations/observability.md)), and a bounded foreground-p99 maintenance guard
  ([maintenance controller](architecture/maintenance-controller.md)). Structured JSON-lines
  lifecycle logging (`--log-format json`) covers start/listen, readiness transitions, shutdown
  drain, maintenance emergency/fault and executor failure.
- Add wire golden fixtures, reserved-bit validation, opcode-specific key/value/expiry constraints,
  stable error mapping and compatibility tests. Duplicate `request_id` guidance (correlation only;
  no server deduplication) and reconnect semantics (re-`INIT`/`BIND_WORKER`; indeterminate
  mutations after transport loss) are specified in
  [wire protocol v2 §8.1 and §10.1](spec/wire-protocol-v2.md) and
  [client semantics v1 §5](spec/client-semantics-v1.md). Protocol versioning remains independent of
  persistence v1.
- Input buffering uses a sliding consumed-prefix cursor and compacts only before an append that
  would otherwise reallocate or cross the configured byte watermark, so deep asynchronous pipelines
  avoid one suffix memmove per completion; contiguous output applies the same rule after partial
  writes, and STATS expose the residual work ([server model](architecture/server-model.md)). One
  bounded scatter lease remains available; a bounded multi-extent output queue remains
  measurement-gated.

### Security and storage namespace

- Write a threat model covering local users, malicious persisted bytes, untrusted clients,
  symlinks/mount replacement, resource exhaustion, supply chain, backups and crash recovery.
  Partial: the [threat model](security/threat-model.md) covers local users, malicious persisted
  bytes, untrusted clients, symlinks, resource exhaustion and crash recovery; it does not yet model
  mount replacement or the supply chain, and covers backups only as credential-file handling.
- Decide whether parent path symlinks are supported. If not, open every component
  descriptor-relatively; on Linux evaluate `openat2` resolution restrictions with a proven `openat`
  fallback.
- Keep exclusive locking explicitly advisory and certify only filesystems whose locking and
  synchronization semantics were tested. Reject or document NFS, SMB, FUSE, overlay and networked
  volumes until certified.
- Add authentication, authorization, TLS/mTLS, secret handling, rate limits, connection/request
  quotas and audit logging before exposing the daemon beyond a trusted loopback/private boundary.
  Secure-profile authn/authz, the Phase 5 abuse controls, Phase 6 auth audit and local CRL
  fail-closed are implemented ([security roadmap](security/roadmap.md)); configure `--tls-crl`
  before exposure to the hostile public Internet. Multi-tenant Phase 8 remains residual, and live
  OCSP over HTTP is unsupported.
- Add dependency and secret scanning, vulnerability-response ownership, fuzz regression intake and
  a supported security maintenance window. Implemented: Trivy filesystem and gitleaks scans
  (`.github/workflows/supply-chain-scan.yml`) and pull-request dependency review
  (`dependency-review.yml`); [SECURITY.md](../SECURITY.md) routes reports privately to the
  maintainer and publishes the prototype support window. Open: fuzz regression intake and formal
  support windows ([security roadmap](security/roadmap.md) items 7.3 and 7.4).

## P2 — platform optimization and engineering evidence

### Platform-specific storage and event algorithms

The current choices are starting points; "optimal" means measured under the same correctness
contract.

| Platform | Keep as baseline | Evaluate and gate with evidence |
| --- | --- | --- |
| Linux | `epoll`, `openat`/`O_NOFOLLOW`, `fallocate`, `fdatasync`, explicit directory `fsync` | `io_uring` only if it lowers tail latency without changing commit ordering; `openat2` path confinement; `preadv2`/batched reads; ext4, XFS, btrfs and container/cgroup matrices |
| macOS | `kqueue`, `F_PREALLOCATE`, ordered `F_BARRIERFSYNC` before the v1 commit slot and `F_FULLFSYNC` for the final persistence point | APFS power-cut evidence, QoS/affinity behavior, bounded mapping/read cache, Apple Silicon performance and Intel compatibility |
| FreeBSD | `kqueue`, descriptor-relative I/O, `posix_fallocate`, `fdatasync`/`fsync` | native UFS/ZFS tests, Capsicum confinement for tools/daemon, CPU affinity and descriptor-cache tuning |
| OpenBSD | `kqueue`, `openat`, eager bounded allocation fallback, conservative `fsync` | native FFS tests, `pledge`/`unveil`, the cost of `fdatasync` being equivalent to `fsync`, and safe lower batch defaults if tail latency requires them |

Linux documents that syncing a file does not necessarily sync its directory entry, and that
writeback can report `EIO`, `ENOSPC` or `EDQUOT` at synchronization time. Apple recommends an
ordered barrier for ordering and reserves full synchronization for stronger persistence needs,
while still describing it as best effort under sudden power loss. OpenBSD documents persistent `EIO`
failure behavior until all references close, and currently implements `fdatasync` through `fsync`.
These differences must remain visible in code, fault tests and operational support rows.

Primary references:

- [Linux `fsync(2)`](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [Apple: Reducing disk writes](https://developer.apple.com/documentation/xcode/reducing-disk-writes)
- [FreeBSD `posix_fallocate(2)`](https://man.freebsd.org/cgi/man.cgi?query=posix_fallocate&sektion=2&manpath=FreeBSD+15.0-RELEASE)
- [OpenBSD `fsync(2)`](https://man.openbsd.org/fsync.2)
- [OpenBSD `open(2)`](https://man.openbsd.org/open.2)

### Data structures and scheduling

- The Swiss slot is 64 rather than 80 bytes on supported 64-bit targets: inline bytes and the heap
  offset share storage, mode and length are packed, a 32-bit hash tag filters candidates, and full
  key bytes remain the collision authority. A 1M-entry ARM64 run reduced RSS by about 31.5 MiB,
  matching 16 bytes across 2,097,152 allocated slots; controlled repetitions and x86-64 evidence
  remain required. Continue benchmarking load factor, control groups, probe lengths, tombstone
  cleanup and transparent lookup. Add scalar/SSE2/AVX2/NEON implementations only with runtime or
  compile-time dispatch and identical property/fuzz results.
- The immutable Reader base uses a 64-byte record with the full hash and a 5-byte-per-bucket
  control/index lookup layout instead of 17 bytes per bucket, and guarded geometric mappings with
  one spare per base lineage. A coherent STATS census and `glifistore_memory_census_benchmark`
  attribute current base/delta payload separately from RSS. The local measurements are in
  [hot-path performance](architecture/hot-path-performance-2026-08-01.md) and the
  [read-generation memory record](../benchmarks/results/local-macos-2026-08-27-read-generation-memory/README.md).
  Total-memory closure remains open: uniquely account shared retired nodes, merge builders, the
  remaining native regions and Segment/thread residency; prove steady state on every platform; and
  define enforceable per-shard budgets before changing the prototype claim ceiling.
- Make group commit deadline-driven and fairness-aware. Record batch occupancy, queue delay, sync
  duration, leader work and per-Worker starvation. Adaptation may change batch size within explicit
  min/max and latency SLOs, never acknowledgement semantics. Threshold and residual groups both
  cross the final commit-slot sync
  ([strict group sync correction](../benchmarks/results/local-macos-2026-08-27-strict-group-sync-fix/README.md)).
  Group coalescing and sync-lane turns are bounded as the
  [mutation lifecycle](spec/mutation-lifecycle.md) specifies, with deterministic litmus coverage and
  turn-split and close-reason telemetry. Adaptive service-cost slack, multi-hour adversarial
  fairness and native multi-device/platform evidence remain open.
- Evaluate bounded read-ahead and mapping by access pattern. Never map unvalidated offsets and never
  let an unbounded mapping/descriptor cache become the memory policy.
- Partition compaction by live ratio and write cost; use a priority queue or tiered policy with
  explicit maximum space amplification. Prevent a hot key range from starving cold reclamation.
- Evaluate NUMA-local Workers, allocator arenas, CPU affinity and cache-line layout on multi-socket
  Linux. Defaults must remain portable; topology tuning is opt-in until automatically reliable.

### Benchmark contract

- Define workloads for point read/write/erase, mixed Zipf, TTL, rotation, recovery, compaction,
  cold and warm cache, sealed-Segment random reads, disk-full approach, network pipelining, slow
  clients, reconnect and long-running memory stability.
- Report throughput with p50/p95/p99/p99.9, queue and sync time, RSS/peak RSS, disk bytes, write
  amplification, batch occupancy, recovery time and confidence/variance. Fix hardware, governor,
  filesystem, mount options, compiler, commit, temperature/warmup and concurrency.
- Establish regression gates separately for throughput and tail latency. GitHub-hosted measurements
  are smoke/regression signals, not publishable cross-machine claims.
- Decide whether machine-local benchmark results belong in Git. If retained, require a schema,
  hardware/compiler/filesystem metadata, source commit, reproducible command and retention policy;
  otherwise publish them as CI/release artifacts.

### Test and CI matrix

- Run fuzzers, do not only compile them. Retain seed/regression corpora for Record, protocol,
  Manifest, Segment header/commit, namespace and recovery state machines.
- Add coverage generation and a risk-based gate; add nightly stress/soak/chaos and
  rotation/compaction workloads. Keep unit, integration, compatibility, crash, filesystem, network
  and performance labels independently runnable. Implemented: weekly diagnostic coverage
  (`.github/workflows/coverage.yml`), a weekly 30-minute soak (`ops-runbooks.yml`) and a monthly
  1-hour soak (`soak-extended.yml`). Open: a risk-based gate, since the
  [test strategy](development/test-strategy.md) keeps line coverage diagnostic, and nightly stress,
  chaos and rotation/compaction workloads.
- Add native FreeBSD and OpenBSD jobs, Linux ARM64, macOS Intel while available, and explicit
  Release tests. Use emulation only for codec portability; it is not storage certification. Decide
  and test the support policy for big-endian and 32-bit readers. Implemented: FreeBSD 14.3
  (`freebsd.yml`) and OpenBSD 7.9 (`openbsd-libressl.yml`) build-and-test jobs in hosted VMs,
  `ubuntu-24.04-arm` rows in `ci.yml` and the `ci.yml` Release test jobs (`release-optimized`,
  `release-lto-smoke`). Open: macOS Intel and the big-endian and 32-bit reader policy.
- Partition sanitizer work so ASan/UBSan, TSan and crash matrices are independently attributable.
  Add MSan only with a fully instrumented supported toolchain.
- Add filesystem jobs for ext4/XFS/btrfs, APFS and UFS/ZFS/FFS where available, with mount metadata
  in artifacts. Use `scripts/collect-durability-evidence.sh` for attributable E2 process-kill
  artifacts and `scripts/run-e3-block-reset.sh` for disposable linux-ext4 / macOS APFS block-reset
  rehearsal (always `e3_certified=no` until a reviewed pinned campaign). Add disk quota and tiny
  block-device jobs.

### Build, hardening, and supply chain

- Apply PIE to executable link steps, not only compilation, and verify PIE, RELRO, immediate
  binding, stack protection and fortification in CI. Make `_FORTIFY_SOURCE=3` configuration-aware
  and prove the optimized build actually enables it.
- Pin third-party Actions to verified full commit SHAs; full SHA is GitHub's immutable-action
  recommendation. Pin Python build dependencies and runner/compiler images, then use an automated,
  reviewed update process. Implemented for Actions: `engineering/tools/validate_actions_pins.py`
  enforces full-SHA pins in `assurance.yml` and `actionlint.yml`, and Dependabot proposes weekly
  updates under the [Actions pinning](security/actions-pinning.md) rules. Open: Python build tools
  (`cmake`, `ninja`, `PyYAML`, `jsonschema`) install unpinned, and jobs use floating runner images
  such as `ubuntu-latest`.
- Upload CodeQL/SARIF to code scanning when repository settings permit it instead of preserving
  only an artifact. Fail or warn on unexpectedly low analyzed-source coverage.
- Checksums and an SPDX SBOM for packaged SDK artifacts are gated in CI
  (`.github/workflows/supply-chain.yml`, `SYFT_REQUIRED=1`). Tag pushes keyless-sign blobs with
  Cosign/Sigstore (`.cosign.bundle`), and SDK tag provenance (Cosign + GitHub attestations,
  public / `ENABLE_ARTIFACT_ATTESTATIONS`) is gated. Tag, dispatch and weekly runs verify two-pass
  digests and a second Linux builder (`ubuntu-22.04`) against the primary sums. Residuals:
  non-Linux release hosts if added later; full SLSA L3 and project GPG.
- Test installation/uninstallation, static/shared policy, symbol visibility, consumer
  exceptions/RTTI compatibility and the declared compiler/standard-library matrix. Continue to avoid
  an ABI promise before 1.0.

Primary supply-chain references:

- [GitHub secure use of Actions](https://docs.github.com/en/actions/reference/security/secure-use)
- [GitHub artifact attestations and SBOM](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations)
- [GitHub CodeQL result upload](https://docs.github.com/en/code-security/tutorials/customize-code-scanning/upload-results)

## P3 — release lifecycle and later capabilities

- Define alpha, beta, release-candidate and stable support periods with upgrade, downgrade,
  deprecation, end-of-life and incident-response policies.
- Produce signed source and supported-platform binary releases with reproducible provenance and a
  compatibility suite that consumes earlier released v1 artifacts.
- Add online resharding, replication, high availability or distributed consensus only as separate
  projects with their own failure models. None is required to finish single-node persistence v1.
- Consider zero-copy pinned reads only after durable compaction has correct reader-generation
  accounting. Consider a persistent derived-index checkpoint only after plain v1 log recovery is
  fully bounded and serves as the independent truth.

## Ordered execution blocks

1. Correctness closure: P0-02 through P0-07, stable error outcomes, explicit close and complete
   fault injection. This freezes what a v1 acknowledgement and recovery mean.
2. Durable lifecycle: P0-08, TTL reclamation, verification, backup/restore and large-store
   resource bounds.
3. Durable daemon: P0-01, asynchronous completion, protocol outcomes, graceful drain, readiness and
   overload behavior.
4. Security and operations: namespace threat model, access control/TLS, logs, metrics, runbooks and
   supported filesystem policy.
5. Platform optimization: architecture-specific Index/poller/storage experiments under the full
   correctness suite and controlled tail-latency benchmarks.
6. Release evidence: native platform matrix, running fuzz/soak/power-cut suites, compatibility
   artifacts, hardened reproducible builds, SBOM, signatures and provenance.

No later block may weaken an earlier gate. In particular, daemon work does not bypass embedded
correctness, and performance work does not alter persistence ordering without a reviewed ADR and
new crash/power-loss evidence.

## Definition of production-ready v1

Persistence v1 is production ready when all P0 items are complete; every supported platform and
filesystem has automated recovery evidence; all resources have tested limits; backup/restore and
verification are usable; the daemon, if shipped as durable, acknowledges exactly the documented
commit state; security and operational controls exist; and released v1 artifacts pass forward and
backward compatibility tests within the published policy.
