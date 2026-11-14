# Platform durability evidence matrix

**Roadmap** · persistence v1, every storage mode claiming restart durability · reviewed 2026-08-30

What each kind of test can establish, and what GlifiStore has established on each platform and
filesystem row. A hosted runner whose filesystem, mount options, cache path and reset mechanism are
unknown gives useful regression evidence, but not storage certification.

The normative byte layouts and restart decisions stay in [persistence v1](../spec/persistence-v1.md)
and the [recovery state-transition matrix v1](../spec/recovery-state-matrix-v1.md). This matrix
governs evidence claims, not recovery behavior.

## Evidence levels

| Level | Mechanism | What it establishes | What it does not establish |
|---|---|---|---|
| E0 | Codec, unit, model, and ordinary reopen tests | Local invariants, canonical bytes, validation, deterministic recovery logic | OS write ordering, process death, device persistence |
| E1 | Deterministic filesystem/allocation fault injection | Exact pre/post syscall outcome, retry, fail-closed behavior, recovery oracle | Kernel/filesystem/device behavior under abrupt loss |
| E2 | `SIGKILL` of the Store harness or real daemon, followed by reopen | Software ordering survives immediate process termination at named checkpoints | Dirty kernel page-cache loss, controller-cache loss, torn sectors, physical power loss |
| E3 | Abrupt VM/block-device reset or controlled physical power cut on a pinned storage row | Recovery under loss below the process/kernel boundary for that exact row | Other kernels, filesystems, mount options, devices, firmware, or cache policies |
| E4 | Repeated release campaign with retained, checksummed provenance and zero unexplained outcomes | Release evidence for the exact supported row and source artifact | A general promise for untested storage stacks |

Levels are cumulative for a certified row: E4 requires the relevant E0–E3 evidence. Passing a
higher level once does not erase missing lower-level coverage. Emulation may prove codec
portability, but it cannot promote storage evidence beyond the layer whose persistence behavior is
actually under test.

## Current evidence

**No platform/filesystem row is E3 or E4 certified.** The repository has E1 coverage, E2
crash harnesses, an attributable E2 collector, and an in-repo E3 block-reset harness for the first
pinned rehearsal rows (Linux ext4 loopback; macOS APFS disk image). Harness PASS is rehearsal
evidence only. Retained release-grade E3/E4 campaign artifacts do not exist yet.

| Platform/filesystem row | Automated evidence available | Highest defensible current claim | Evidence path placeholder | Missing before certification |
|---|---|---|---|---|
| macOS / APFS | Native build/test workflow; E2 collector; `scripts/run-e3-block-reset.sh --platform macos-apfs` (hdiutil sparsebundle + `detach -force`); scheduled CI harness smoke | E2 when a collector artifact records an actual passing native run; E3-harness rehearsal when the APFS diskimage campaign artifact is retained — **not E3 certified** | [`engineering/evidence/platform-durability/macos-apfs/`](../../engineering/evidence/platform-durability/macos-apfs/) | Pinned Apple hardware/storage (not only a disk image on a hosted runner), APFS and mount metadata, reviewed abrupt-loss campaign with repetitions, then E4 |
| Linux / ext4 | Hosted Linux workflow; E2 collector; `scripts/run-e3-block-reset.sh --platform linux-ext4` (sparse image + losetup + dm-flakey); PR drop-write smoke and scheduled three-mode fault rehearsal | E2 only for a separately recorded run on a declared ext4 mount; E3-harness rehearsal on loopback/mapper — **not E3 certified** for production NVMe/SATA ext4 | [`engineering/evidence/platform-durability/linux-ext4/`](../../engineering/evidence/platform-durability/linux-ext4/) | Dedicated ext4 block device or VM disk (beyond loopback-on-hosted-FS), mount/cache/barrier metadata, per-request block trace or equivalent reset proof, reviewed campaign, then E4 |
| Linux / XFS | Code and suites are portable to the row; no pinned row in current CI | E1 implementation coverage; E2/E3 row evidence not retained | [`engineering/evidence/platform-durability/linux-xfs/`](../../engineering/evidence/platform-durability/linux-xfs/) | Native XFS job, E2 run, reset campaign and artifacts |
| Linux / btrfs | Code and suites are portable to the row; no pinned row in current CI | E1 implementation coverage; E2/E3 row evidence not retained | *(not scaffolded; row remains open)* | Native btrfs job, E2 run, reset campaign and artifacts |
| FreeBSD / UFS | Native FreeBSD VM workflow builds and runs the general suite (`.github/workflows/freebsd.yml`); guest storage row is not pinned | Portability/regression signal, not UFS certification | [`engineering/evidence/platform-durability/freebsd-ufs/`](../../engineering/evidence/platform-durability/freebsd-ufs/) | Pin UFS mount metadata, E2 collector artifact, then E3/E4 campaign |
| FreeBSD / ZFS | No maintained native storage row; FreeBSD CI does not select or report ZFS | Architectural target, not a supported durability claim | [`engineering/evidence/platform-durability/freebsd-zfs/`](../../engineering/evidence/platform-durability/freebsd-zfs/) | Explicit ZFS contract, native ZFS job, E2, then pinned E3/E4 campaign |
| OpenBSD / FFS | OpenBSD/LibreSSL VM workflow runs the general suite, but does not pin or report the guest storage row as durability evidence | Portability/regression signal, not FFS certification | [`engineering/evidence/platform-durability/openbsd-ffs/`](../../engineering/evidence/platform-durability/openbsd-ffs/) | Native or controlled FFS row, filesystem metadata, E2 and E3/E4 artifacts |
| NFS, SMB, FUSE, overlay, remote or user-space storage | Deliberately outside the local-filesystem contract | Unsupported | n/a | Not eligible for certification under persistence v1 |

“Hosted workflow” never means that its provider’s current backing filesystem has been inferred or
certified. A row advances only from an artifact that names the tested stack. Loopback and disk-image
rows must be labeled as such in provenance (`guest_host_boundary=…`).

## Evidence artifact contract

Every retained E2–E4 artifact must contain:

- schema version, UTC start/end, exact source commit, clean/dirty source status, and build profile;
- OS release and kernel, architecture and hardware/virtualization class;
- filesystem type, mount point and options, backing device/image class, and relevant cache/barrier
  policy;
- compiler, CMake/CTest, test executable identity, exact command, repetition count, and exit status;
- checkpoint or reset schedule, recovery oracle, and all unexpected outcomes;
- SHA-256 manifest for the artifact contents.

E3/E4 also require the reset mechanism, the guest/host boundary, the controller or virtual-device
cache policy, the boot and filesystem health result, and confirmation that the reset was not a
graceful shutdown. Physical device identifiers and credentials must not be stored; stable redacted
labels are sufficient.

An artifact from a dirty worktree is diagnostic only. An artifact with missing filesystem or reset
metadata cannot be promoted by relabeling it by hand. Evidence is attached to the tested commit and
row; it does not transfer automatically to later source, OS, kernel, firmware or mount changes.

## Safe native E2 collector

The repository collector records provenance and can run all six embedded and daemon process-kill
tests:

```sh
scripts/collect-durability-evidence.sh \
  --output /path/to/new/evidence-directory \
  --build-dir build/macos-debug \
  --probe-path . \
  --run process-kill \
  --repeat 3
```

The collector refuses to overwrite an existing directory. It records only E0 metadata or E2
process-kill evidence and always emits `power_loss_exercised=no`. It never resets a machine,
detaches a block device, changes a mount or claims E3/E4. During an E2 run it sets `TMPDIR` to the
writable probe directory, so the crash harness creates its temporary data on the filesystem being
described.

For a quick inventory without running tests:

```sh
scripts/collect-durability-evidence.sh \
  --output /path/to/new/metadata-directory \
  --probe-path . \
  --metadata-only
```

In CI, `.github/workflows/durability-evidence.yml` archives:

- metadata on every change;
- process-kill evidence on the weekly schedule or manual dispatch, including a retained,
  reproducible randomized E2 crash/reopen campaign;
- E3 harness smoke on pull requests;
- a weekly three-mode Linux block-fault matrix;
- APFS campaign rehearsal;
- a hosted-ci E0→E3 orchestrator path.

It never labels an upload as power-loss certification (`scripts/assert-e3-rehearsal-honesty.sh`).

## E3 block-reset harness

For the first pinned rehearsal rows, `scripts/run-e3-block-reset.sh` provisions a **disposable**
filesystem row, arms an external reset below the process boundary at named persistence checkpoints,
and asks the crash hook to `SIGSTOP` the worker instead of using the ordinary E2 `SIGKILL`. The
controller independently confirms the stopped state before faulting or detaching the row; a missing
confirmation is `INCONCLUSIVE`. It then kills the stopped worker, runs an offline filesystem check
without repair (`-n`; APFS is temporarily reattached and unmounted for this step), remounts, and
evaluates the same recovery oracle as `glifistore_crash_persistence`.

Preferred first-row paths:

| Row label | Provisioning | Reset mechanisms |
|---|---|---|
| `linux-ext4` | sparse image → `losetup` → `mkfs.ext4` → mount; optional `dm-flakey` mapper | `abrupt-detach` (lazy umount + `losetup -d`); `dm-flakey` with `drop-writes`, `error-writes`, or `all-io-error`, then force-remove |
| `macos-apfs` | `hdiutil` APFS sparsebundle attached at a private mountpoint | `hdiutil detach -force` |

```sh
# Linux ext4 loopback smoke (requires sudo for loop/dm/mount/fsck)
scripts/run-e3-block-reset.sh \
  --output /path/to/new/e3-artifact \
  --build-dir build/unix-debug \
  --platform linux-ext4 \
  --profile smoke \
  --reset-mechanism dm-flakey \
  --dm-fault-mode error-writes

# macOS APFS disk-image smoke
scripts/run-e3-block-reset.sh \
  --output /path/to/new/e3-artifact \
  --build-dir build/macos-debug \
  --platform macos-apfs \
  --profile smoke \
  --reset-mechanism abrupt-detach
```

Profiles:

- `smoke` — `put` checkpoints `write_record`, `sync_record`, `write_commit_slot`, `sync_commit_slot`
- `campaign` — smoke plus `bootstrap sync_commit_slot`, `rotate sync_commit_slot#2`, and compaction
  intent/promotion checkpoints (`write_compaction_intent`, `rename_segment`, `sync_directory#3`)

Checkpoint markers live on the host (`host-scratch/`), while the Store data directory lives on the
test volume, so a block reset cannot erase the arming evidence. For a macOS detach, prefer the
mounted APFS volume node from the `hdiutil attach` output, not the first `/dev/disk*` line, which is
often the GUID partition scheme. The default image sizes (1 GiB smoke, 2 GiB campaign) exceed the
Store’s default 256 MiB free-space reserve plus Segment preallocation.

Every harness artifact sets `e3_certified=no` and `physical_power_cut=no`. Per-case results keep the
requested checkpoint action, independent worker-stop confirmation, reset mechanism, dm-flakey mode,
reset confirmation, fsck status, recovery result and outcome. A green CI job means the harness and
recovery oracle rehearsed successfully on that disposable row, not that GlifiStore is certified for
sudden power loss on production hardware.

### dm-flakey notes (Linux)

- Requires root, `dmsetup`, and a loop (or real) block device underneath.
- Keep a linear mapper fully available while seeding and reaching the checkpoint. At the armed
  reset, the mapper is suspended with `--noflush` and reloaded as `dm-flakey`, and `--dm-fault-mode`
  selects `drop-writes` (silently discard writes), `error-writes` (fail writes) or `all-io-error`
  (no optional feature, so all I/O fails during the down interval), with the semantics in the
  [Linux kernel dm-flakey documentation](https://cdn.kernel.org/doc/html/latest/admin-guide/device-mapper/dm-flakey.html).
- The harness confirms that the fault table was armed, holds a bounded 250 ms window, and confirms
  mapper removal. After that window it lazy-unmounts the faulted filesystem, terminates the already
  stopped worker to release its open files, and accepts reset confirmation only after the mapper is
  gone. It does **not** claim that a particular dirty write reached the mapper during that window;
  the absence of a block-level I/O trace remains a stated limitation of this rehearsal row.
- Confirm mapper removal separately from “reload succeeded.” A timeout, lost console, or failed
  remove is **INCONCLUSIVE**, not PASS.
- dm-flakey on loopback-over-ext4-on-hosted-disk still inherits the host’s durability; treat it as
  block-layer rehearsal for the named loop/mapper row, not as NVMe firmware certification.

## E3 / E4 PASS–FAIL criteria

### E3 case outcome (single checkpoint × repetition)

| Outcome | When |
|---|---|
| **PASS** | Worker stop and reset confirmed below the process boundary; remount succeeds; `fsck -n` (or platform equivalent) runs without silent repair; recovery oracle accepts the required/optional/forbidden visibility for that checkpoint |
| **FAIL** | Reset confirmed, but reopen/oracle disagrees with the recovery matrix, or the Store corrupts committed state that must survive |
| **INCONCLUSIVE** | Checkpoint or worker stop not confirmed, reset not confirmed, host/harness crash, lost console, missing non-interactive fsck privilege/tool, or fsck/remount infrastructure failure |

Failed and inconclusive rows are retained; they are never discarded to beautify a campaign.

### E3 row claim (promotion)

A storage row may be labeled **E3 certified** only when all of the following hold:

1. Exact row identity is pinned (OS/kernel, FS type, mkfs/mount options, backing device/image class,
   cache/barrier policy, guest/host boundary, reset mechanism).
2. Clean source commit; E0/E1/E2 already passed on that same row.
3. Campaign covers the normative persistent checkpoints for the claimed scope (at least the smoke
   set for a narrow put-scope claim; full recovery-matrix scope for a general durable claim).
4. Each checkpoint has enough repetitions to cover timing variability; zero unexplained outcomes.
5. Artifact matches the evidence contract, including SHA-256 manifest and explicit
   `reset_confirmed=yes` per case.
6. A release maintainer reviews and records the artifact reference in the release notes.

Until then, keep `e3_certified=no` even if the harness exits 0.

### How to promote (human gate only)

No filesystem row in this matrix is marked E3/E4 certified today. Do **not** check those claims from
CI green, harness exit 0, or an agent-produced tarball alone.

The operator package (campaign preparation, always `e3_certified=no` until this gate completes):

- Runbook: [E3 pinned-row campaign](../operations/e3-campaign.md)
- Orchestrator: `scripts/run-e3-campaign.sh` (E0 → E1 → E2 → E3, evidence tarball + SHA-256
  manifest)

Promote a row to **E3 certified** only when all of the following are true:

1. Maintainer completed every box in the artifact’s `promotion-checklist.md`.
2. Pin identity matches the intended claim (`hardware_class`, `fs_pin`, `guest_host_boundary`,
   mount/cache notes). Hosted CI and disposable loopback/diskimage boundaries cannot certify
   production NVMe/SATA firmware rows.
3. Campaign result is `passed`, `source_dirty=no`, no skipped E0/E1/E2/E3 stages, repetitions meet
   the runbook minima, and unexplained INCONCLUSIVE/FAIL counts are zero.
4. Release notes cite the retained tarball SHA-256 and `pin_label`.
5. A release/storage maintainer edits **this** matrix row’s “Highest defensible current claim”
   column and the opening “No platform/filesystem row is E3 or E4 certified” sentence only for the
   rows actually promoted.

Scripts must keep emitting `e3_certified=no`. Relabeling provenance by hand without steps 1–5 is
invalid.

### E4 row claim

E4 also requires a repeated release campaign on the same pinned row and source artifact lineage,
retained provenance for every attempt, and zero unexplained outcomes across the campaign set. E4 is
never granted by CI smoke alone.

## E3 campaign protocol

Power-loss work must run only in a disposable, controlled environment with no unrelated data on the
target device or image. Automation is platform-specific, but every campaign follows the same
sequence:

1. Provision a fresh named filesystem row and record the image/device, filesystem creation options,
   mount options, cache mode, host/guest boundary, and software versions.
2. Build a clean, immutable source commit and first pass E0, E1, and E2 on that exact row.
3. For every persistent checkpoint in the normative recovery matrix, arm an external reset trigger.
   The trigger must act below the process boundary and must not allow an orderly shutdown.
4. Restart, capture filesystem health/fsck output without silently repairing away evidence, open the
   Store, and evaluate the checkpoint’s required/optional/forbidden visibility oracle.
5. Repeat each checkpoint enough times to cover timing variability. Record every attempt, including
   harness failures and inconclusive resets; do not discard failed rows.
6. Checksum and retain the complete artifact. Promote to E4 only through the release checklist and
   only for the exact tested row.

The preferred operator entry point for steps 1–6 (still `e3_certified=no`):

```sh
scripts/run-e3-campaign.sh \
  --output /path/to/new/e3-campaign \
  --build-dir build/unix-debug \
  --pin-label lab-host-ext4-loop-a \
  --hardware-class diskimage-rehearsal \
  --fs-pin ext4 \
  --guest-host-boundary loopback-image-on-host-filesystem \
  --platform linux-ext4 \
  --repeat 10 \
  --e2-repeat 3
```

The campaign controller must distinguish “reset command issued” from “power was actually removed.”
A timeout, host crash, lost console or test-harness error is inconclusive, not a passing recovery.
Physical chassis power cuts remain the strongest E3 mechanism for hardware rows; loopback and
disk-image resets are valid only for the exact virtual-device row they name.

## Promotion and regression rules

A storage row is supported only when its exact scope and E4 artifact are published with the release.
Any change to write ordering, synchronization primitives, Manifest or intent publication,
commit-slot handling, recovery authority, filesystem policy or the power-loss harness invalidates
the affected campaign. Kernel, filesystem and device changes require review and usually a new
campaign.

CI may archive E2 collector output for every change, E3 harness smoke for the linux-ext4 rehearsal
row on pull requests, and weekly campaign-profile and orchestrator rehearsal artifacts. E3
certification campaigns should run on dedicated lab pins (not hosted-ci), and E4 is a reviewed
release campaign. Until those gates exist, GlifiStore remains experimental for durable deployment,
even when every process-kill and harness-smoke test passes.
