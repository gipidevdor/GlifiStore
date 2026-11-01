# Architecture Decision Records

ADRs preserve why a durable architectural choice was made. They are historical records, not mutable
implementation guides.

## Lifecycle

Allowed statuses are:

- `proposed`: under review and not authoritative;
- `accepted`: authoritative unless superseded;
- `amended`: still authoritative together with named later ADRs;
- `superseded`: retained for history but no longer governs new work;
- `rejected`: considered and deliberately not selected;
- `deprecated`: still implemented temporarily but scheduled for removal.

An accepted ADR is never silently rewritten to pretend a later decision existed. Add `Amended by`,
`Superseded by`, or a new consequences section that clearly identifies the later decision.

## Required structure

Use [template.md](template.md). Every new ADR must describe context, decision drivers, considered
alternatives, decision, consequences, compatibility impact, verification, and relationships.

## Index

| ADR | Decision | Status | Relationships |
|---|---|---|---|
| [0001](0001-project-scope.md) | Narrow native exact-key project scope | accepted | — |
| [0002](0002-fixed-segment-size.md) | Fixed 64 MiB Segments | accepted | — |
| [0003](0003-log-indexed-storage.md) | Append-only Records plus positional Index | accepted | — |
| [0004](0004-index-as-derived-state.md) | Index is rebuildable acceleration state | accepted | — |
| [0005](0005-worker-auto-sizing.md) | Automatic creation-time Worker sizing | accepted | depends on 0006 for durable ownership; concurrency amends 0031/0032 |
| [0006](0006-key-routing-hash.md) | Persisted FNV-1a-64 routing v1 | accepted | amended by 0024, 0030 (keyed SipHash) |
| [0007](0007-swiss-table-index.md) | SwissTable-style Worker Index | accepted | detailed by Index v1 specification; mix seed configurable (0026) |
| [0008](0008-alpha-persistence-contract.md) | Base volatile/sync/periodic persistence contract | amended | amended by 0010 and 0011 |
| [0009](0009-public-read-ownership.md) | Owning public reads; pinned reads reserved | accepted | pinned API remains unimplemented; concurrency notes amended by 0032 |
| [0010](0010-durable-periodic-policy.md) | Periodic durability policy | amended | amended by 0011 batching |
| [0011](0011-durable-group-commit.md) | Strict and periodic group batching | accepted | amends 0008 and 0010 |
| [0012](0012-worker-affine-reactors.md) | One Reactor/executor per Worker; one-time connection handoff | amended | amended by 0031: Reactor is the Reader half of a mandatory pair |
| [0013](0013-native-wire-protocol-v2.md) | Explicit native binary protocol v2 | accepted | exposes 0006 and 0012 |
| [0014](0014-crc32c.md) | CRC32C Castagnoli parameters and role | accepted | persistent-format dependency |
| [0015](0015-whole-worker-compaction.md) | Whole-Worker sealed-history durable compaction | amended | preserves 0003/0004/0008; amended by 0039 |
| [0016](0016-bounded-cold-read-executor.md) | Bounded async durable cold reads outside Reactors | accepted | amends 0012; preserves 0013 ordering |
| [0017](0017-bounded-durable-hot-cache.md) | Bounded durable hot cache with pinned active fallback | accepted | preserves 0008; builds on 0016 |
| [0018](0018-bounded-durable-mutation-lanes.md) | Bounded Worker-affine durable mutation lanes | amended | replaced incrementally by the one-Writer SPSC lane in 0031 |
| [0019](0019-client-error-retry-timeout.md) | Official client error/retry/timeout contract | accepted | detailed by client semantics v1; depends on 0013 |
| [0020](0020-tls-outer-transport.md) | TLS 1.3 outer transport for secure profile | accepted | LibreSSL first-class on OpenBSD; enables 0021 |
| [0021](0021-secure-profile-authentication.md) | mTLS authentication for secure profile | accepted | depends on 0020; enables 0022 |
| [0022](0022-authorization-capabilities.md) | Coarse read/write/admin capabilities | accepted | depends on 0021; amended by 0025 |
| [0023](0023-maintenance-controller.md) | Optional budgeted MaintenanceController | accepted | preserves 0015; schedules `Store::compact()` |
| [0024](0024-offline-worker-migration.md) | Offline Worker reshard + 0.x compatibility | accepted | amends 0005, 0006, 0008; amended by 0033 (online design deferred) |
| [0025](0025-key-prefix-tenant-scope.md) | Optional authz key-prefix scope (Phase 8 slice) | accepted | amends 0022; amended by 0027 (STATS) |
| [0026](0026-keyed-index-hash-seed.md) | Keyed Index mix seed (hash-flood slice) | accepted | amends 0007; keyed Worker routing in 0030 |
| [0027](0027-stats-isolation-prefix-principals.md) | STATS requires admin for prefix principals | accepted | amends 0022 and 0025 |
| [0028](0028-per-tenant-data-dir-deferred.md) | Per-tenant data-dir isolation | proposed | deferred; do not half-break single Store |
| [0029](0029-uds-peercred.md) | UDS transport + optional peercred principals | accepted | amends 0021; Phase 8 local authn |
| [0030](0030-keyed-worker-routing.md) | Keyed Worker routing (SipHash + Manifest seed) | accepted | amends 0006, 0024, 0026 |
| [0031](paired-reader-writer-shards.md) | Shard obbligatori a coppie Reader–Writer | accepted | modello obbligatorio per 0.1.0; amends 0005, 0012, 0016, 0018, 0023, 0030; amended by 0032 |
| [0032](0032-paired-concurrency-embedded-store.md) | Paired concurrency for embedded Store | accepted | amends 0031, 0005, 0009 (concurrency notes); supersedes mutex default |
| [0033](0033-online-rebalance-deferred.md) | Online rebalance design constraints (deferred) | accepted | amends 0024; not implemented in 0.1.x |
| [0034](0034-zero-fence-hot-backup-deferred.md) | Zero-fence hot backup design constraints (deferred) | accepted | extends backup-restore v1 non-goal; not implemented in 0.1.x |
| [0035](0035-generation-shell-recycling.md) | PairReadGeneration shell TLS freelist under existing publish protocol | rejected | measured affine PUT regression; does not amend 0031; full slot pool → 0036 |
| [0036](0036-generation-slot-pool-publish.md) | Generation slot-pool publish/reclaim design bar | proposed | would amend 0031 publication/reclaim after V1–V14; depends on 0031/0032; related 0035 |
| [0037](0037-shard-execution-token-flat-combining.md) | Shard execution token + flat combining | accepted | amends 0031/0032 Writer identity; phased embedded→daemon windows |
| [0038](0038-stable-c-abi-artifact-delivery.md) | Stable C ABI + candidate verification architecture | accepted | C ABI independent from product/wire/storage versions; C++ ABI unchanged |
| [0039](0039-pre-intent-staged-compaction-output.md) | Stage and verify compaction output before the v1 publication lease | accepted | amends 0015/0023; amended by 0040; no persistent-format change |
| [0040](0040-pre-intent-compaction-write-pacing.md) | Pace bounded private compaction writes before durable authority | accepted | amends 0023/0039; no persistent-format change |
| [0041](0041-bounded-network-security-lifecycles.md) | Bounded TLS/output lifetimes, denied-attempt quotas and audit emission | accepted | amends 0012/0019/0020/0022; preserves v1/v2 and Store ACK points |

Official TCP client error taxonomy, automatic retries, and deadline behavior are governed by
[ADR 0019](0019-client-error-retry-timeout.md) and [client semantics v1](../spec/client-semantics-v1.md).
Embedded Store `ErrorCode` values remain a separate surface.

Secure network profile (TLS / mTLS / capabilities) is governed by
[ADR 0020](0020-tls-outer-transport.md), [ADR 0021](0021-secure-profile-authentication.md),
[ADR 0022](0022-authorization-capabilities.md), [ADR 0025](0025-key-prefix-tenant-scope.md),
[ADR 0026](0026-keyed-index-hash-seed.md), [ADR 0027](0027-stats-isolation-prefix-principals.md), and
[ADR 0029](0029-uds-peercred.md);
implementation order is in [security roadmap](../security/roadmap.md).
