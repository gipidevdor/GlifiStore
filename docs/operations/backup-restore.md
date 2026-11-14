# Backup and restore

**Descriptive** · durable data directories: `manifest.glifi`, catalog Segments · reviewed 2026-07-31

How to make a verified copy of a Store for migration, disaster recovery or pre-change safety, and
how to restore it: a restore is the same verified copy into a new empty destination. The normative
boundary is [backup and restore v1](../spec/backup-restore-v1.md); the tool reference is
[`glifistore_backup_store`](../cli.md#glifistore_backup_store).

| Path | When | Mechanism |
| --- | --- | --- |
| Offline CLI | Stopped Store or stopped `glifistored` | Exclusive data-directory lock through `glifistore_backup_store` |
| Online API | Open durable `Store` or live `glifistored` | `Store::backup_to` or wire `BACKUP` (opcode 10) |

## Requirements

Each of these fails closed:

- Offline CLI: stop every writer before a backup or restore. The tool takes an exclusive Store lock
  and fails if `glifistored` or another `Store` holds the directory.
- Online API: the Store stays open and keeps the lock. Mutations are briefly fenced, so it is not a
  fully concurrent hot copy. Prefer the offline CLI for cold release backups.
- The destination must be empty (`create_new`). A non-empty destination fails with
  `sequence_conflict` or `invalid_argument`.
- No in-place restore over the production directory. Always copy into a new path, verify, then swap
  at the orchestration layer (rename, mount or service pointer), never by overwriting open files.
- A copy holds only the recovery-safe catalog Segments and `manifest.glifi`, which is copied last.
  Crash temporaries, compaction intents and bootstrap intents are not copied.

## Back up offline

The offline path is preferred for release artifacts.

### 1. Quiesce writers

```bash
systemctl stop glifistored
# confirm no process holds the data dir
```

### 2. Verify the structure (optional, recommended)

```bash
glifistore_verify_store -- /var/lib/glifistore
glifistore_verify_store --json -- /var/lib/glifistore
```

Exit `1` means: do not back up until you have followed the
[corruption and repair runbook](corruption-repair.md).

### 3. Back up into a new empty directory

```bash
install -d -m 700 /backup/glifistore-2026-07-23
glifistore_backup_store -- /var/lib/glifistore /backup/glifistore-2026-07-23
```

For a JSON audit trail:

```bash
glifistore_backup_store --json -- /var/lib/glifistore /backup/glifistore-2026-07-23
```

For a faster header-only pass that skips the committed Record CRC scan:

```bash
glifistore_backup_store --no-scan -- /var/lib/glifistore /backup/glifistore-2026-07-23
```

Prefer the full scan for release backups. `--no-scan` is for repeated operator checks when header
and commit validation is sufficient.

### 4. Confirm success

- The exit code is `0`.
- The tool has verified the destination after the copy: lock, verify source, copy catalog files,
  sync, verify destination.
- Keep backup artifacts outside the live data directory, on a separate volume or in object storage.

## Back up online

Use `Store::backup_to(destination)` on an open durable Store, `Server::backup_to(destination)` in
process, wire opcode `BACKUP` (10) against a live `glifistored`, or a typed client
`backup(destination)` (C++ and the official SDKs). On the wire the key is the UTF-8 destination
path, and `BACKUP` requires `admin` under `--authz-map`.

Expect a short admission fence. In-flight operations drain, and new operations return `unavailable`
during the flush, the structural source check and the catalog Segment and Manifest copy. On the
wire, PUT, ERASE and GET arriving during the fence are refused before Store entry with
`OVERLOADED`: a temporary admission pause, not `INTERNAL_ERROR`. Writers resume before the
destination verify (optional CRC).

While the Store holds the lock, an external `glifistore_backup_store` against the same path still
fails with `io_error`. Online backup remains fenced, not zero-impact hot I/O.

## Restore

### 1. Prepare a new empty data directory

```bash
install -d -m 700 /var/lib/glifistore-restored
```

### 2. Copy from the backup

Run the same tool with the paths reversed:

```bash
glifistore_backup_store -- /backup/glifistore-2026-07-23 /var/lib/glifistore-restored
```

### 3. Open through recovery

Point `glifistored`, or an embedded `Store::open(..., open_existing)`, at the restored directory.
Recovery rebuilds the Indexes from the Manifest and the committed Records; no separate Index rebuild
tool is required.

```bash
glifistored --profile production --data-dir /var/lib/glifistore-restored --bind 127.0.0.1 --port 7379
```

Confirm `READY` before serving traffic.

### 4. Cut over

Stop the old instance, switch the service atomically to the restored path (symlink, mount or
configuration update), start the daemon and confirm `READY` again.

## Failure modes

| Condition | Result | Operator action |
| --- | --- | --- |
| Source locked (offline CLI) | `io_error` (already locked) | Stop writers and retry, or use `Store::backup_to` |
| Source fails verify | Same error as `glifistore_verify_store` | Follow the corruption runbook; do not treat the backup as valid |
| Destination exists or is non-empty | `sequence_conflict` or `invalid_argument` | Choose a new empty path |
| Copy I/O failure midway | Fails closed; the destination may be incomplete | Delete the incomplete destination and retry |

## What not to do

- Do not run `glifistore_backup_store` against a live data directory for convenience.
- Do not treat online fenced backup as zero-impact under load; schedule it or use the offline CLI.
  Zero-fence in-process backup is deferred
  ([ADR 0034](../adr/0034-zero-fence-hot-backup-deferred.md)); volume snapshots remain an external
  operator option.
- Do not restore by copying files by hand without the verify and sync ordering. The tool keeps the
  catalog Segment and Manifest publication order that the implementation contract requires.
