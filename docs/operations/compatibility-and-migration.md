# Compatibility and migration

**Normative** · persistence v1, wire v2, Worker migration, released fixtures · reviewed 2026-08-01

How to reopen, upgrade, downgrade and change the Worker count of a durable Store before 1.0. The
procedures are normative for 0.1.x and descriptive for unreleased majors. The machine-readable
policy is [`n-n1-matrix.yaml`](../../engineering/compatibility/n-n1-matrix.yaml); the lifecycle
rules are in [version lifecycle](../architecture/version-lifecycle.md) and the
[N↔N-1 matrix](../architecture/n-n1-compatibility.md). The claim ceiling stays architectural
prototype until the alpha gates close with retained evidence.

| Goal | Supported | Procedure |
| --- | --- | --- |
| Reopen the same directory with the same Worker count on 0.1.x | Yes | Stop writers, verify, open |
| Change the Worker count | Offline only | [`glifistore_migrate_store`](#change-the-worker-count) into a new directory |
| Open a pre-v1 or unknown required format | No | Fails closed |
| Rewrite a newer required version for a downgrade | No | Not promised |
| Serve a non-v2 wire client on 0.1.x | No | Rejected by design |
| Keep the C++ ABI stable across 0.x builds | No | Rebuild consumers (`SameMinorVersion`) |
| Migrate live or reshard online | No | Out of scope for v1 |

## Reopen on the same line

Preconditions: persistence format v1, unchanged Worker count, every writer stopped.

```bash
# Stop glifistored and close embedded Stores: the directory lock must be free.
glifistore_verify_store -- /path/to/data
# Start the new binary with the same Worker count (omit the override, or pass the persisted count).
glifistored --profile production --data-dir /path/to/data ...
```

A binary that requires a higher format version than the Store encodes fails closed. Reopen never
rewrites the Manifest or Segments, never changes the Worker count and never repairs corruption in
place. Matrix row: `STORE-SAME-LINE`.

## Change the Worker count

Ordinary reopen cannot change the persisted Worker count; an offline logical copy into a new
directory can ([store migration](../architecture/store-migration.md),
[ADR 0024](../adr/0024-offline-worker-migration.md)). It needs disk space for a second full copy of
the live data, a fully stopped source and a target count `N` in `[1, 256]` that matches the
daemon's `--shard-pairs` (`--workers` is the 0.1.x alias).

1. Verify the source: `glifistore_verify_store -- /var/lib/glifistore`.
2. Optionally back it up:
   `glifistore_backup_store -- /var/lib/glifistore /var/backups/glifistore-$(date +%Y%m%d)`.
3. Migrate: `glifistore_migrate_store --workers 4 -- /var/lib/glifistore /var/lib/glifistore-w4`.
4. If interrupted, run the same command again; it resumes from
   `/var/lib/glifistore-w4.migrate-state`.
5. Verify the destination: `glifistore_verify_store -- /var/lib/glifistore-w4`.
6. Only after the destination verifies, move traffic to it:
   `glifistored --data-dir /var/lib/glifistore-w4 --shard-pairs 4 ...`.
7. Keep the source until the destination has soaked under traffic and the rollback window has
   ended, then retain or delete it deliberately.

The source is never mutated; a resume always uses `<destination>.migrate-state`. Matrix row:
`STORE-WORKER-RESHARD` (`supported_offline_only`).

## Downgrade and future formats

| Situation | Action |
| --- | --- |
| Older binary, Store still pure persistence v1 | Verify, then reopen with the same Worker count if every required version is implemented; otherwise it fails closed |
| Store encodes a newer required version | Do not force an open; restore a backup taken before the upgrade |
| An older line is needed for salvage | Offline backup or export only, never a silent reopen (`STORE-DOWNGRADE-REWRITE` is `not_promised`) |

## Wire clients

0.1.x supports wire protocol v2 only (`WIRE-V2-SAME`); other frames are rejected before service
(`WIRE-V1-OR-UNKNOWN`). Cross-major wire compatibility is `not_promised` until an explicit matrix
row and fixtures exist. The official SDKs speak v2. After a server upgrade, roll clients only when
the release notes and fixtures keep the wire row supported.

## Released fixtures

`tests/fixtures/released/self-v1/` proves the harness. For each version tag:

```bash
./scripts/package-release-compatibility-artifacts.sh 0.1.0
./scripts/package-release-claim.sh 0.1.0 "$(git rev-parse HEAD)"
# Commit tests/fixtures/released/0.1.0/ when promoting a permanent N−1 drop (release decision).
```

`.github/workflows/release-compat.yml` packages self and tag artifacts; committing permanent
prior-release trees under `tests/fixtures/released/<label>/` stays a maintainer release step (see
[released fixtures](../../tests/fixtures/released/README.md)).

A complete Store baseline is separate from those codec vectors. After a tagged release has created
and verified a stopped Store through its installed daemon, package it with
`engineering/tools/persistence_fixture.py create` under `tests/fixtures/released-stores/<semver>/`,
following the metadata and digest procedure in that directory's README. The next release's
artifact-only persistence job requires this baseline and rejects a self-generated candidate Store.

## Back up before upgrading

Take a verified backup before any binary upgrade that touches persistence: offline with the
[backup runbook](backup-restore.md), or online with `Store::backup_to` or wire `BACKUP` (a short
admission pause, not a zero-fence hot copy). Zero-fence hot backup is not supported.

Release steps are in the [release checklist](../assurance/release-checklist.md).
