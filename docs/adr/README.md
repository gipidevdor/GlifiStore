# Architecture decision records

An ADR records why a durable architectural choice was made. This directory holds only decisions
that are **in force** or **open**. Rejected and fully superseded ADRs are removed from the tree and
remain available in Git history; their numbers are never reused.

## Lifecycle

| Status | Meaning |
| --- | --- |
| `proposed` | Under review; not authoritative |
| `accepted` | Authoritative |
| `amended` | Authoritative together with the named later ADRs |

When a later ADR fully replaces an earlier one, the earlier file is deleted in the same change and
every link to it is repointed. A partial change is recorded with `Amends` / `Amended by`; an accepted
ADR is never rewritten to pretend a later decision existed. New ADRs use [template.md](template.md).

Removed: 0018 (replaced by 0031), 0035 (rejected; see
[rejected optimizations](../architecture/hot-path-rejected-optimizations.md)), 0043 (replaced by 0044).

## Records

### Scope, storage and format

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0001](0001-project-scope.md) | Narrow native exact-key scope | accepted | |
| [0002](0002-fixed-segment-size.md) | Fixed 64 MiB Segments | accepted | |
| [0003](0003-log-indexed-storage.md) | Append-only Records plus positional Index | accepted | |
| [0004](0004-index-as-derived-state.md) | Index is rebuildable derived state | accepted | |
| [0007](0007-swiss-table-index.md) | SwissTable-style Worker Index | accepted | seed by 0026 |
| [0014](0014-crc32c.md) | CRC32C parameters and role | accepted | persistent format |
| [0042](0042-large-blob-storage.md) | External blobs up to 64 MiB, opt-in persistence v2 | accepted | wire unchanged |

### Durability, recovery and compaction

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0008](0008-alpha-persistence-contract.md) | Volatile / sync / periodic persistence contract | amended | by 0010, 0011 |
| [0010](0010-durable-periodic-policy.md) | Periodic durability policy | amended | by 0011 |
| [0011](0011-durable-group-commit.md) | Strict and periodic group batching | accepted | rejected siblings: 0047 |
| [0047](0047-durable-group-rejected-siblings.md) | Rejected siblings keep the strict group | accepted | hazard HAZ-044 |
| [0015](0015-whole-worker-compaction.md) | Sealed-history durable compaction | amended | by 0039, 0044 |
| [0039](0039-pre-intent-staged-compaction-output.md) | Stage and verify output before the publication lease | accepted | amended by 0040, 0044 |
| [0040](0040-pre-intent-compaction-write-pacing.md) | Pace private compaction writes | accepted | |
| [0044](0044-bounded-prefix-compaction-remap.md) | Oldest-prefix compaction with commit-time remap | accepted | |
| [0034](0034-zero-fence-hot-backup-deferred.md) | Zero-fence hot backup constraints | accepted | deferred |

### Concurrency, runtime and reads

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0031](paired-reader-writer-shards.md) | Mandatory paired Reader–Writer shards | accepted | amended by 0032, 0037 |
| [0048](0048-volatile-tcp-response-borrow.md) | Encode small volatile TCP values within a generation lease | accepted | reduces copies; HAZ-045 stays open |
| [0049](0049-adaptive-read-capacity.md) | Manage capacity with bounded adaptive read helpers | accepted | opt-in volatile prototype; capacity and platform qualification pending |
| [0032](0032-paired-concurrency-embedded-store.md) | Paired concurrency for the embedded Store | accepted | |
| [0037](0037-shard-execution-token-flat-combining.md) | Shard execution token and flat combining | accepted | |
| [0005](0005-worker-auto-sizing.md) | Creation-time Worker sizing | accepted | amended by 0024, 0031 |
| [0012](0012-worker-affine-reactors.md) | Worker-affine Reactors, one-time handoff | amended | by 0016, 0031 |
| [0009](0009-public-read-ownership.md) | Owning public reads | accepted | |
| [0016](0016-bounded-cold-read-executor.md) | Bounded cold reads outside Reactors | accepted | 0045 proposes per-connection pipelining |
| [0017](0017-bounded-durable-hot-cache.md) | Bounded durable hot cache | accepted | 0045 proposes images for paired mode |
| [0023](0023-maintenance-controller.md) | Budgeted MaintenanceController | accepted | |

### Routing, protocol and clients

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0006](0006-key-routing-hash.md) | Persisted FNV-1a-64 routing v1 | accepted | amended by 0024, 0030 |
| [0030](0030-keyed-worker-routing.md) | Keyed SipHash Worker routing | accepted | |
| [0024](0024-offline-worker-migration.md) | Offline Worker reshard | accepted | online design: 0033 |
| [0033](0033-online-rebalance-deferred.md) | Online rebalance constraints | accepted | deferred |
| [0013](0013-native-wire-protocol-v2.md) | Native binary protocol v2 | accepted | |
| [0019](0019-client-error-retry-timeout.md) | Client error, retry and timeout contract | accepted | [client semantics v1](../spec/client-semantics-v1.md) |
| [0038](0038-stable-c-abi-artifact-delivery.md) | Stable C ABI and candidate verification | accepted | C++ ABI unchanged |

### Security

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0020](0020-tls-outer-transport.md) | TLS 1.3 outer transport | accepted | |
| [0021](0021-secure-profile-authentication.md) | mTLS authentication | accepted | |
| [0022](0022-authorization-capabilities.md) | Read / write / admin capabilities | accepted | amended by 0025, 0027 |
| [0025](0025-key-prefix-tenant-scope.md) | Key-prefix authorization scope | accepted | |
| [0026](0026-keyed-index-hash-seed.md) | Keyed Index hash seed | accepted | |
| [0027](0027-stats-isolation-prefix-principals.md) | STATS requires admin for prefix principals | accepted | |
| [0029](0029-uds-peercred.md) | UDS transport and peer-credential principals | accepted | |
| [0041](0041-bounded-network-security-lifecycles.md) | Bounded TLS/output lifetimes, quotas, audit | accepted | |

Implementation order for the secure profile: [security roadmap](../security/roadmap.md).

### Open proposals

| ADR | Decision | Status | Notes |
| --- | --- | --- | --- |
| [0045](0045-memory-resident-durable-reads.md) | Memory-resident durable reads | proposed | amended by 0046; gate `GATE-DURABLE-READS` |
| [0046](0046-unified-memory-reserve.md) | One memory reserve: pageable Index, resident images | proposed | implemented locally; gate `GATE-DURABLE-READS` |
| [0036](0036-generation-slot-pool-publish.md) | Generation slot-pool publish/reclaim | proposed | V1–V14 matrix |
| [0028](0028-per-tenant-data-dir-deferred.md) | Per-tenant data-directory isolation | proposed | deferred |
