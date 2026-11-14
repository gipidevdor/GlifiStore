# Backup and restore v1

**Normative** · offline CLI, online fenced `Store::backup_to`, wire `BACKUP` · reviewed 2026-08-01

This specification freezes the **consistency boundary** and supported copy/restore procedures for
durable persistence v1. Operator steps are in the
[backup and restore runbook](../operations/backup-restore.md).

## 1. Snapshot boundary

A successful backup is a **verified catalog snapshot**:

1. Every Segment named by a valid `manifest.glifi` for the Store, and
2. That Manifest file itself (written **last**).

The snapshot is consistent for recovery if and only if destination verify succeeds after copy.
Indexes are **not** copied; `Store::open(..., open_existing)` rebuilds them via recovery.

### Included

- Catalog Segment files referenced by the Manifest (mode `0600` on Unix copies)
- `manifest.glifi` (final file in the destination)

### Excluded (must not appear in a conforming backup)

- Crash temporaries, compaction intents, bootstrap intents
- Non-catalog namespace anomalies
- Volatile-only runtime state
- Live TCP sessions / in-memory hot cache

## 2. Consistency points by path

| Path | Writer activity during copy | Consistency point |
| --- | --- | --- |
| Offline (`glifistore_backup_store`, embedded `backup_durable_store`) | None — exclusive data-dir lock; fail if locked | Post-verify source → copy → post-verify destination |
| Online fenced (`Store::backup_to` / wire `BACKUP`) | New admissions fenced for flush + structural source check + catalog copy; destination CRC verify runs after admissions resume | Flush → catalog exclusive lock → structural source verify → parallel Segment copy → Manifest last → resume admissions → verify destination (optional CRC) |

Online fenced backup is **not** a zero-fence hot copy and is **not** a filesystem freeze/COW
orchestrator. Zero-fence concurrent hot backup remains out of scope for v1; design constraints for
any future path are frozen in [ADR 0034](../adr/0034-zero-fence-hot-backup-deferred.md). Shortening
the admission fence so destination verify is outside the fence is an allowed incremental change to
the fenced path (still not zero-fence).

Concurrent online backup callers own counted admission fences. A caller waiting for exclusive
catalog-copy ownership must retain its fence until its own copy completes; releasing another
caller's fence cannot reopen admission. The counter/transition serialization is confined to backup
and lifecycle control and is not part of the normal GET/PUT admission path.

## 3. Offline procedure (normative steps)

1. Ensure no Store/`glifistored` holds the source directory lock.
2. Exclusive-lock source; run `verify_durable_store` (Manifest, namespace audit, catalog Segment open;
   optional CRC scan unless `--no-scan`).
3. Create destination with `create_new` (empty). Non-empty destinations fail closed.
4. Copy catalog Segments, then `manifest.glifi` last; `fsync`/`fdatasync` files and destination dir
   per platform persistence rules used by the implementation.
5. Release locks; `verify_durable_store` destination. Exit non-zero ⇒ do not put destination in
   service.

Restore is the same verified copy from a backup directory into a **new empty** destination. There
is no in-place overwrite of an open production directory.

## 4. Online fenced procedure (normative steps)

1. Reject if the Store is not durable.
2. Fence new admissions; wait for in-flight admitted work (bounded by close/admission rules).
3. Flush durable state per the Store's durability policy.
4. Under catalog exclusive lock: **structural** source verify (Manifest/namespace/Segment open; no
   committed CRC scan) + copy catalog Segments (bounded parallel) then `manifest.glifi` last.
5. Release catalog lock and **resume admissions** (writers may proceed).
6. Verify destination independently before promoting it (optional committed CRC scan). Destination
   verify is the promotion gate; the live source is not CRC-rescanned after resume.

Wire `BACKUP` (opcode 10): key = UTF-8 destination path; empty value; `expire_at_ns = 0`;
`target_worker = kNoWorker`. Success value is a bounded ASCII report including `files_copied`,
`bytes_copied`, and timing needles `admission_fence_ns`, `catalog_copy_ns`,
`destination_verify_ns`. Secure profile requires `admin`. Official SDKs expose typed `backup`/
`Backup` helpers.

## 5. Failure modes

| Condition | Result |
| --- | --- |
| Offline tool, source locked | Fail closed (`io_error` / already locked) |
| Source fails verify (offline full CRC; online structural) | Same error as `verify_durable_store` |
| Destination fails verify (incl. CRC when scanning) | Fail closed; do not promote destination |
| Destination exists / non-empty | Fail closed (`sequence_conflict` or `invalid_argument`) |
| Mid-copy I/O failure | Fail closed; destination must not be served without re-verify |
| Online backup on volatile Store | `invalid_argument` |
| Incomplete destination opened for service | Operator error — forbidden |

## 6. Explicit non-goals

- Fully concurrent hot backup with zero admission fencing (see [ADR 0034](../adr/0034-zero-fence-hot-backup-deferred.md))
- In-place destructive repair of the source
- Preserving crash temporaries or compaction intents
- Filesystem freeze / COW snapshot orchestration as a GlifiStore feature (operators may use
  external volume snapshots; that is not this API)

## 7. Tooling

```bash
glifistore_backup_store [--json] [--no-scan] -- /path/to/source /path/to/backup
glifistore_backup_store [--json] [--no-scan] -- /path/to/backup /path/to/restored
```

```cpp
store->backup_to("/path/to/empty/backup");
server->backup_to("/path/to/empty/backup");
```

## Related

- [Corruption repair](../operations/corruption-repair.md)
- [Compatibility and migration](../operations/compatibility-and-migration.md)
- Requirement / gate linkage: `GS-OPS-BACKUP-001`, `GATE-BACKUP-RESTORE`
