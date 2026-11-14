# Corruption and repair

**Descriptive** · durable v1 data directories and Segment files · reviewed 2026-07-23

How to classify a suspected storage fault, preserve the evidence and, when salvage is possible,
produce a clean verified copy of the Store, without destructive repair in place. Ordinary
`Store::open` recovery never mutates, truncates or rewrites source files
([durability and recovery](../architecture/durability-recovery.md)). Diagnosis uses read-only tools;
salvage uses `glifistore_repair_store` into an explicit empty workspace, with quarantine outside the
live store tree.

| Symptom | First step |
| --- | --- |
| The daemon fails to open the Store, or `READY` reports a sticky fault | Stop the daemon; run `glifistore_verify_store` on its `--data-dir` |
| A known bad Segment file | Run `glifistore_inspect_segment` on the file |
| Extra files after a crash (orphan Segments, notes) | Verify, then repair into a workspace if verify passes except for quarantinable anomalies |
| A missing catalog Segment, symlink or hard link | Fails closed: repair refuses; restore from backup |

## 1. Stop writers and verify

```bash
systemctl stop glifistored
glifistore_verify_store --json -- /var/lib/glifistore
```

[`glifistore_verify_store`](../cli.md#glifistore_verify_store) is read-only. It takes the exclusive
Store lock, so it fails while the daemon still runs. It decodes `manifest.glifi`, audits the
namespace, opens each catalog Segment read-only against the Manifest identity and scans the
committed Record extents unless `--no-scan` is given.

Exit codes: `0` validated, `1` validation or I/O failure, `2` usage. Exit `1` fails closed: do not
open the Store for service until the fault is classified.

For a header-only check while iterating:

```bash
glifistore_verify_store --no-scan -- /var/lib/glifistore
```

## 2. Inspect individual Segments (optional)

When verify points at a specific catalog file, or you hold a detached Segment copy:

```bash
glifistore_inspect_segment --json -- segment-0123456789abcdef-00000001.glifi
glifistore_inspect_segment --no-scan -- segment-0123456789abcdef-00000001.glifi
```

The tool does not take the Store directory lock. A concurrent writer can cause a torn commit
observation, which it reports as a validation failure, not as success.

## 3. Repair offline into a workspace

Repair applies when verify fails because of quarantinable namespace anomalies (unlisted Segments,
crash temporaries, compaction intents, unknown regular files) while the catalog Segments listed in
the Manifest are present and safe. It requires:

- an unchanged source directory: repair never mutates the source;
- a completely empty workspace directory;
- writable space for `<workspace>/store` and `<workspace>/quarantine`.

```bash
install -d -m 700 /srv/glifistore-repair-2026-07-23
glifistore_repair_store --json -- /var/lib/glifistore /srv/glifistore-repair-2026-07-23
```

On success the workspace contains:

| Path | Contents |
| --- | --- |
| `<workspace>/store/` | Clean Manifest and catalog Segments, verified after copy |
| `<workspace>/quarantine/` | Non-catalog anomalies moved out of the catalog view |
| `<workspace>/quarantine/audit.txt` | Operator audit trail |

Open the repaired Store only from `<workspace>/store`:

```bash
glifistore_verify_store -- /srv/glifistore-repair-2026-07-23/store
glifistored --data-dir /srv/glifistore-repair-2026-07-23/store ...
```

Preserve `<workspace>/quarantine/` for forensics. Do not copy quarantined files back into the
catalog namespace.

## When repair refuses

Repair exits `1` and produces no usable `<workspace>/store` when:

- a catalog Segment listed in the Manifest is missing;
- the namespace contains unsafe entries (symlinks, hard links, non-regular objects);
- the workspace is not empty;
- structural verify of the salvageable catalog fails.

In these cases restore from the last good offline backup ([backup and restore](backup-restore.md))
or engage the maintainers. Do not attempt manual catalog surgery in the production directory.

## Corruption or incompatibility

| Class | Examples | Tool behavior |
| --- | --- | --- |
| Corruption | Bad CRC in a committed extent, missing listed Segment, inconsistent sequences | Verify and repair fail closed |
| Incompatibility | Unknown required format version | Fails closed as incompatibility, not as generic repair |
| Benign crash debris | Unlisted Segment file, recognized temporary | Quarantined by repair; the source is untouched |

## What not to do

- Do not edit, truncate or `rm` files in the production data directory to fix recovery.
- Do not run repair while `glifistored` holds the lock.
- Do not run repair with a non-empty workspace.
- Do not attempt live or hot repair: it is not supported.
- Do not invoke `glifistore_rebuild_index` for durable v1: durable Indexes rebuild through ordinary
  Store recovery, and the tool exits `1` with an explicit error.
- Do not place quarantine directories inside the live store catalog path; use the explicit
  workspace layout above.

## After the incident

1. Keep the original source directory, read-only, as evidence.
2. Keep `<workspace>/quarantine/` and the JSON or text output of verify.
3. Verify the repaired or restored Store before traffic: `glifistore_verify_store`, then `READY`.
4. Find the root cause: filesystem, hardware, abrupt power loss or operator error
   ([platform durability evidence](../architecture/platform-durability-evidence.md)).

## Command summary

```bash
glifistore_verify_store   [--json] [--no-scan] -- <DATA-DIR>
glifistore_inspect_segment [--json] [--no-scan] -- <SEGMENT-FILE>
glifistore_repair_store   [--json] [--no-scan] -- <SOURCE-DIR> <EMPTY-WORKSPACE>
glifistore_backup_store   [--json] [--no-scan] -- <SOURCE-DIR> <EMPTY-DEST>   # when salvage impossible
```
