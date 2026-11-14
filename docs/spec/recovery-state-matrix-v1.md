# Recovery state-transition matrix v1

**Normative** · persistence format v1 and durable Store recovery · reviewed 2026-07-23

The restart outcome of every interrupted persistent transition in format v1.

## 1. Purpose and terminology

This specification fixes the recovery result for every persistent transition implemented by
GlifiStore v1: Store bootstrap, mutation/flush, active-Segment rotation, and compaction of the
oldest sealed prefix of one Worker ([ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md)).
It refines the ordering rules in [persistence v1](persistence-v1.md) without changing any
encoded byte.

The following terms are used throughout:

- **authority**: the validated Manifest whose catalog is allowed to define recovered Store state;
- **old authority** (`Mold`): the Manifest selected before a transition;
- **next authority** (`Mnext`): the one-generation successor admitted by a rotation or compaction;
- **selected slot**: the valid Segment commit slot with the greatest commit generation;
- **present**: the mutation must be visible after recovery;
- **absent**: the mutation must not be visible after recovery;
- **optional**: either complete old or complete new state is permitted because the process stopped
  after a write reached the kernel but before its durability boundary completed;
- **resume**: recovery may perform only the exact idempotent completion or cleanup named here;
- **fail closed**: open returns an error before serving traffic and does not adopt, truncate, or
  reinterpret untrusted state.

Temporary files never become authority. A filename never overrides validated Store ID, Segment ID,
generation, Worker owner, role, checksum, or Manifest membership.

## 2. Recovery dispatch order

After acquiring the exclusive directory lock, open evaluates persistent protocols in this order:

| Order | Durable marker | Required action |
|---:|---|---|
| 1 | canonical bootstrap intent | Validate the canonical initial Manifest and complete bootstrap |
| 2 | canonical compaction intent | Select exactly `Mold` or `Mnext`, recover it fully, then finish transaction cleanup |
| 3 | no intent; Manifest active entry selects a sealed Segment | Complete the one exact interrupted rotation |
| 4 | none of the above | Perform ordinary Manifest-driven recovery |

A marker is actionable only when its complete checksum, version, Store identity, generations, and
embedded catalog constraints validate. Conflicting markers, an authority outside the admitted set,
or unrelated namespace entries fail closed.

Recovery must validate the selected authority and all of its listed Segments before deleting any
non-authoritative object. It must finish all required work and perform the final namespace audit
before publishing the Store as ready.

## 3. Store bootstrap

The bootstrap intent is a complete canonical initial Manifest v1. It is the sole transaction marker
until every initial active Segment exists and the intent has been durably removed.

| Persistent state at restart | Authority | Recovery action | Result |
|---|---|---|---|
| no Manifest or intent; pristine directory | none | `open_or_create` may start a new bootstrap; `open_existing` returns `not_found` | no Store is inferred by `open_existing` |
| only `.glifistore.bootstrap.tmp` may remain | none | ignore/replace the private temporary during a new `open_or_create` bootstrap | new bootstrap receives a new canonical intent |
| valid bootstrap intent; no Manifest | intent payload | publish the exact intent Manifest | resume |
| valid intent; matching Manifest; some initial Segments absent | matching Manifest | create only missing exact initial Segment identities | resume |
| valid intent; matching Manifest; all initial Segments present and pristine | matching Manifest | durably remove the intent | completed Store |
| intent removal was attempted but directory sync did not complete | intent if present, otherwise Manifest | repeat completion or ordinary recovery | completed Store |
| valid intent and a different Manifest | none | fail closed as corruption | no mutation |
| present initial Segment has a wrong identity or is not pristine | none | fail closed as corruption | no adoption or replacement |
| Manifest exists without intent but a listed initial Segment is missing | Manifest is invalid as a complete authority | fail closed as corruption | no implicit bootstrap repair |

The canonical initial catalog has Manifest generation 1, routing epoch 1, one active generation-1
Segment per Worker, Segment IDs `1..worker_count`, and `next_segment_id = worker_count + 1`.
Bootstrap completion may create a missing Segment only while the matching intent remains present.

The process-kill bootstrap matrix exercises these ordered boundaries:

```text
create data directory -> sync parent
-> write/sync/rename bootstrap intent -> sync directory
-> write/sync/rename Manifest -> sync directory
-> preallocate/write/sync/rename each initial Segment -> sync directory
-> remove bootstrap intent -> sync directory
-> first Record write/sync/slot write/slot sync
```

## 4. Record commit and flush

Record bytes outside the selected committed extent have no logical meaning. Only a valid selected
commit slot can authorize a larger extent.

| Last completed boundary before process stop | Selected durable boundary after restart | Mutation visibility |
|---|---|---|
| validation or encode only | old slot | absent |
| Record write | old slot | absent |
| Record data/order sync | old slot | absent |
| alternate slot write, before successful slot sync | old or new slot | optional |
| alternate slot sync completed | new slot | present |
| in-memory Index publication or acknowledgement after slot sync | new slot | present |

A restart never scans an uncommitted tail. If the new slot survives, every Record it authorizes must
decode and checksum correctly; otherwise recovery fails closed rather than falling back past
committed corruption.

### 4.1 Storage-mode consequences

| Mode | Acknowledgement point | Crash before slot durability | Crash after acknowledgement |
|---|---|---|---|
| `durable_sync` | after the mutation slot sync | absent/optional according to the table above | present |
| `durable_group` | after the whole batch slot sync and in-memory batch publication | unacknowledged batch is absent/optional | every acknowledged batch member is present |
| `durable_periodic` | after Record write and in-memory publication, before required slot sync | an acknowledged unflushed mutation may be absent or present | present only if a later periodic, explicit, or shutdown flush completed |

`Store::flush()` and orderly `Store::close()` force pending periodic and partial group batches
through the same slot boundary. A flush failure before slot write is not committed. Failure while
synchronizing a written slot is indeterminate; a fresh open resolves exactly one old or new selected
slot and therefore one absent or present result. The failed runtime remains fail-closed.

The sequence number restored for a Worker is `maximum committed sequence + 1`. Uncommitted tails do
not consume recovered sequence space. Exhaustion at `UINT64_MAX` fails open.

## 5. Active-Segment rotation

Rotation has no separate file intent. Its durable marker is the combination of `Mold` still naming
an active entry while that exact Segment's selected persisted state is already `sealed`.

| Persistent state at restart | Authority | Recovery action | Result |
|---|---|---|---|
| preflight failed before sealing | `Mold` | ordinary recovery | old active remains writable |
| seal slot was written but not synced | `Mold` | selected slot may remain active or become sealed | ordinary recovery or exact completion |
| sealed state is durable; no replacement exists | `Mold` | create the exact `next_segment_id`/generation as a pristine active Segment | resume |
| sealed state is durable; only a stale replacement temporary exists | `Mold` | ignore the temporary and create/validate the exact final replacement | resume |
| sealed state is durable; exact final replacement exists and is pristine | `Mold` | adopt only that replacement for the transition | resume |
| replacement is durable; next Manifest publication has not selected `Mnext` | `Mold` | publish the canonical rotation successor | resume |
| atomic Manifest replacement may have selected `Mold` or `Mnext` | selected complete Manifest | complete from `Mold`, or recover `Mnext` ordinarily | one complete authority |
| `Mnext` is authoritative | `Mnext` | old entry is sealed and replacement is active; scan both normally | rotation complete |
| original mutation has begun on the replacement | `Mnext` | apply the Record/slot matrix in section 4 | absent, optional, or present |

The canonical successor changes only these catalog facts:

- the old active entry becomes sealed;
- the exact previous `next_segment_id`/generation is appended as active for the same Worker;
- Manifest generation and `next_segment_id` increment once.

An unlisted exact-next Segment is admissible only with exactly one sealed-active marker and only
when pristine. An unrelated orphan, a non-pristine replacement, multiple sealed-active Workers, a
sealed Manifest entry whose persisted state is active, or any identity mismatch fails closed.

## 6. Oldest-prefix compaction

The compaction intent embeds both exact authorities. Before the intent is durable, `Mold` is the
only authority. Once the intent is durable, recovery admits exactly `Mold` or `Mnext`.

| Persistent state at restart | Authority | Recovery action | Result |
|---|---|---|---|
| planning or prepared Index only | `Mold` | ordinary recovery; no persistent transaction exists | no compaction |
| staged output creation/copy/seal/verification; no durable intent | `Mold` | scan `Mold` completely, then remove recognized private Segment temporaries and sync the directory | no compaction; clean namespace |
| only `.glifistore.compaction.tmp` exists | `Mold` | ignore the private temporary during ordinary recovery | no compaction |
| valid intent exists; no replacement final names | `Mold` | validate `Mold`, then remove intent | rollback complete |
| valid intent; replacement temporaries or exact final replacements exist | `Mold` | validate `Mold`; remove both exact temporary and final non-authoritative replacement names; sync; remove intent | rollback complete with clean namespace |
| replacement Record/slot/seal work is partial while `Mold` remains authoritative | `Mold` | same rollback; never adopt replacement contents | rollback complete |
| Manifest publication may have selected `Mold` or `Mnext` | exact selected authority | branch only by full Manifest equality | deterministic rollback or commit |
| `Mnext` is authoritative; all old sources still exist | `Mnext` | validate `Mnext`; retire exact old sources; remove intent | commit complete |
| `Mnext` is authoritative; old-source retirement is partial | `Mnext` | accept already missing obsolete names; validate and retire the remainder | resume commit cleanup |
| old-source directory sync was indeterminate | `Mnext` | repeat idempotent retirement and sync | resume commit cleanup |
| intent removal failed before unlink | `Mnext` | repeat removal after validating authority | resume commit cleanup |
| intent was unlinked but final directory sync was indeterminate | `Mnext` | ordinary recovery if intent is absent; otherwise repeat cleanup | committed authority preserved |
| Manifest equals neither embedded authority | none | fail closed as corruption | no cleanup |
| authoritative Segment missing or invalid | none | fail closed as corruption | no cleanup |
| unrelated or unsafe namespace entry exists | none | fail closed as corruption | no adoption |

Recovery under either authority first performs the complete ordinary Segment scan. Only then may it
remove non-authoritative names. Every removal validates the expected immutable Segment identity;
missing obsolete names are accepted because cleanup is idempotent. No old source may be removed
before `Mnext` and its directory entry are durable.

The process-kill matrix covers pre-intent replacement preallocation/header/Record copy, Record
synchronization, data and seal commit slots, intent write/sync/rename, the post-intent directory
sync, replacement promotion, Manifest write/sync/rename, both source removals, intent removal, and
all five directory-sync positions. Its online compaction seed is a deterministic 30-operation
history containing PUT, overwrite, ERASE, expired PUT, and active-Segment decisions across eight
keys; every checkpoint reopens and checks the complete sequential model rather than only sentinel
keys. Two additional multi-output cleanup scenarios SIGKILL after each of two replacement removals
under `Mold` and each of three source removals under `Mnext`, including their retirement and intent
directory synchronizations. Each subsequent ordinary reopen must finish the remaining cleanup
without changing the selected authority. An online 3-to-2 scenario adds differential checkpoints for
second-replacement preallocation/header/promotion, the batched promotion directory sync, the 64th of
80 maximum-size Record writes (the first into the second output), the second output's data and seal
commit slots, the shifted Manifest/retirement/intent directory syncs, and the third source removal.
Before Manifest directory sync, recovery must select `Mold`, remove both final and partial-temporary
replacement identities, and leave all 80 values visible. At and after that sync it must select
`Mnext`, retire all three sources, and preserve the same model. The opt-in `copy-matrix` adds
`write_record#1` through `write_record#63`; the standard matrix owns `write_record#64`. Together
they SIGKILL after Record copies 1 to 64 of 80 and cover 152 distinct persistence checkpoints;
copies 65 to 80 have no kill point of their own. The opt-in `random-matrix` repeats nine
representative old/next-authority checkpoint classes for four fixed seeds. Each 120-operation
history combines PUT, overwrite, ERASE, expired PUT, and restoration, converges to exactly 88
maximum-size live values, and verifies the complete model after each of 36 process kills. The
complementary `random-campaign` mode derives a fresh workload seed and one of those nine checkpoint
classes for every iteration from a recorded decimal campaign seed using a repository-owned
deterministic generator. It supports 1–10,000 local cases, writes a row per reopen to a
non-overwritten TSV report, and keeps the full seed/boundary/outcome schedule reproducible. Hosted
CI deliberately caps manual runs at 512 cases and schedules 256 cases weekly so timeout pressure
cannot silently reduce the oracle matrix.

## 7. Ordinary recovery and rejection matrix

After transaction completion, ordinary recovery applies these rules:

| Observed condition | Classification | Action |
|---|---|---|
| valid uncommitted bytes after selected extent | expected crash tail | ignore |
| malformed/checksum-failed Record inside selected extent | committed corruption | fail closed |
| missing or identity-mismatched listed Segment | committed corruption | fail closed |
| unknown required format version | incompatibility | fail closed |
| overlapping, reversed, or equal conflicting Worker sequence ranges | corruption/sequence conflict | fail closed |
| newest key decision is tombstone or expired | valid hidden value | suppress all older values |
| stale canonical engine temporary permitted by namespace policy | crash residue | report; do not adopt |
| unlisted final Segment outside an active intent transition | namespace corruption | fail closed |
| symlink, hard-linked private file, device, socket, or unknown entry | unsafe namespace | fail closed |
| resource policy too small for bounded recovery | resource exhaustion | fail before service, without rewriting |

Ordinary open does not salvage, truncate, quarantine, or rewrite source data. Backup restoration,
repair, and salvage are separate explicit operator workflows.

## 8. Evidence map

| Evidence | Covered state |
|---|---|
| `glifistore_crash_sync` | 89 distinct SIGKILL checkpoints across bootstrap, sync mutation, rotation, single-output online compaction, differential online 3-to-2 build/publication, and two-output rollback/retirement cleanup |
| `glifistore_crash_persistence --mode copy-matrix` | 63 opt-in multi-output SIGKILL checkpoints which, with the standard matrix, cover Record copies 1 to 64 of 80 and 152 distinct checkpoints total |
| `glifistore_crash_persistence --mode random-matrix` | 36 opt-in recoveries across four reproducible 120-operation multi-output histories and nine authority/copy/cleanup checkpoint classes |
| `glifistore_crash_persistence --mode random-campaign` | bounded reproducible sampling of randomized 120-operation histories × nine checkpoint classes; every SIGKILL is followed by full-model reopen verification and a structured outcome row |
| `glifistore_crash_periodic` | Record write, Record sync, slot write, and slot sync for deferred durability |
| `glifistore_crash_group` | the same four boundaries for strict group commit |
| `glifistore_crash_daemon_sync` / `group` / `periodic` | acknowledged wire mutation survives real-daemon SIGKILL; periodic waits through its flush window |
| [`segment_file_tests.cpp`](../../tests/unit/segment_file_tests.cpp) | alternating slots, uncommitted tails, ambiguous slot sync, committed corruption |
| [`persistence_recovery_tests.cpp`](../../tests/integration/persistence_recovery_tests.cpp) | lifecycle, routing, sequence, mutation/flush faults, exact rotation completion |
| [`compaction_recovery_tests.cpp`](../../tests/integration/compaction_recovery_tests.cpp) | old/next authority selection, idempotent rollback/retirement, and cleanup of a partially created second replacement |
| [`store_tests.cpp`](../../tests/integration/store_tests.cpp) | public bootstrap completion, periodic/group flush, close, and reopen |
| [`PersistenceRecovery.tla`](../../engineering/formal/persistence/PersistenceRecovery.tla) | bounded abstract Record sync → commit-slot sync → Manifest publication → crash/recovery state space, including fail-closed committed corruption |

These tests establish process-termination and deterministic injected-I/O evidence on development and
CI filesystems. The TLA+ model establishes logical safety only under its stated abstract persistence
assumptions. None of these proofs certifies controller power-loss behavior, storage-device write caches,
filesystem-specific guarantees, or cross-release artifacts. Those require the separate platform
durability evidence matrix. The in-repo E3 block-reset harness
(`scripts/run-e3-block-reset.sh`) confirms a paused checkpoint worker before rehearsing abrupt detach
or bounded dm-flakey `drop-writes`, `error-writes`, and `all-io-error` modes on disposable ext4/APFS
image rows. It records reset/remount/oracle outcomes but must not be reported as E3 certification.
