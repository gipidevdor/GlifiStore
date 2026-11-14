# ADR 0024: Offline Worker resharding and release compatibility

- Status: accepted
- Date: 2026-07-23
- Deciders: persistence maintainers
- Applies to: durable persistence v1; Worker routing; 0.x release lifecycle
- Amends: [0005](0005-worker-auto-sizing.md), [0006](0006-key-routing-hash.md), [0008](0008-alpha-persistence-contract.md)
- Amended by: [0033](0033-online-rebalance-deferred.md) (online path design constraints; still deferred)
- Supersedes: none

## Context

Durable Stores persist Worker count and routing metadata in the Manifest. Reopen rejects a
requested Worker count that disagrees with that metadata because `hash(key) % worker_count` would
silently change ownership. The production roadmap requires an offline, resumable, verified v1
migration tool before Worker-count changes are supported, and requires published upgrade/downgrade
rules before alpha durability promises. Online resharding, replication, and consensus are out of
scope for single-node persistence v1.

## Decision drivers

- Preserve exact-key ownership without peer-fallback searches.
- Never rewrite a live data directory in place.
- Make interrupted migration restartable without double-ownership ambiguity.
- Keep persistence format v1 stable across 0.x until an explicit format bump.
- Avoid promising C++ ABI stability before 1.0.

## Alternatives considered

- **Implicit open-time rewrite when Worker count differs.** Rejected: changes ownership during
  recovery, cannot be audited, and conflicts with fail-closed reopen.
- **Online reshard with a routing-slot table.** Deferred: requires a separate failure model,
  dual-ownership windows, and client `routing_epoch` cutover; tracked as P3.
- **Backup plus manual re-PUT scripts.** Rejected as the sole path: not resumable, not verified as
  a product contract, and easy to leave incomplete.

## Decision

1. Reopen remains reopen-only for the same persisted Worker count, routing algorithm, and
   persistence format version. Worker-count changes require an explicit offline migration.
2. Offline migration copies live visible key/value/expiry state into a **new empty destination**
   Store created with the target Worker count. The source directory is never mutated.
3. Migration is resumable through a sibling checkpoint file outside the destination catalog, and
   final success requires destination verification.
4. Persistence v1 bytes are forward-compatible across 0.x readers that declare Manifest/Segment/
   Record v1 support. Newer required format versions fail closed on older binaries.
5. No C++ ABI stability is promised before 1.0. Disk and wire compatibility follow encoded versions,
   not the library SemVer alone.
6. Online Worker resharding remains future work and is not required to finish persistence v1.

## Consequences

Positive: operators have a supported cutover path; reopen stays fail-closed; policy matches ADRs
0005/0006/0008.

Negative: changing Worker count requires downtime and temporary disk for a second Store; sequences
and Store identity are not preserved across migration (destination is a new Store).

Deferred: online reshard, hot migration, cross-release binary matrix against published Git tags,
and downgrade exporters for future format versions beyond v1.

## Compatibility and migration

- Existing Stores continue to reopen unchanged when Worker count matches.
- `glifistore_migrate_store` / `migrate_durable_store` is the supported Worker-count change path.
- Destination gets a new `store_id` and bootstrap `routing_epoch`; clients must cut over to the new
  data directory (and daemon `--data-dir`) after verify.
- Backup/restore remains byte-identical catalog copy and does **not** change Worker count.

## Verification

- Unit tests for 1↔N and N↔1 reshard, resume after interrupt, locked-source refusal, destination
  verify, and reopen Worker-count mismatch.
- CLI help/version smoke tests.
- Golden fixture decode matrix remains the in-tree codec evidence; released-artifact layout is
  documented for tag drops.

## References

- [Version lifecycle and compatibility](../architecture/version-lifecycle.md)
- [Offline Store migration](../architecture/store-migration.md)
- [Worker model](../architecture/worker-model.md)
- [Format compatibility matrix](../architecture/format-compatibility.md)
- [Persistence v1 production roadmap](../v1-production-roadmap.md)
