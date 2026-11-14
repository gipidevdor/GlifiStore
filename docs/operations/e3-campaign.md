# E3 pinned-row campaign

**Descriptive** · the E3 campaign, E3 harness and E2 collector scripts · reviewed 2026-08-30

How a maintainer produces a retained E3 campaign-prep artifact on a declared hardware or VM and
filesystem pin, reviews it and, only when every gate passes, promotes a row. The orchestrator is
`scripts/run-e3-campaign.sh`; it drives the E2 collector `scripts/collect-durability-evidence.sh`
and the E3 harness `scripts/run-e3-block-reset.sh`. A coding agent cannot certify E3 on real NVMe.
The claim rules are in the
[platform durability evidence matrix](../architecture/platform-durability-evidence.md).

**Hard rule.** Every automated artifact keeps `e3_certified=no` until a human completes the
sign-off checklist and updates the evidence matrix and the release notes. A green campaign exit code
is not certification. CI (`.github/workflows/durability-evidence.yml`) exercises the harness smoke,
a weekly campaign-profile loopback and diskimage rehearsal, and an optional hosted-ci E0→E3
orchestrator path; all of them remain rehearsal. `scripts/assert-e3-rehearsal-honesty.sh` fails
closed if an artifact claims E3 certification or release-certification eligibility.

## What a campaign establishes

| If the pin is… | A passing campaign may support… | It does not establish… |
| --- | --- | --- |
| Hosted CI or unknown backing store | Harness regression only | Any production storage claim |
| Loopback or APFS diskimage (even on an NVMe host) | E3-harness evidence for that image row | NVMe firmware or controller-cache certification |
| VM dedicated virtio/NVMe disk with the Store data on that disk | An E3 row claim for that exact guest disk and filesystem, after review | Other kernels, filesystem options or hosts |
| Bare-metal NVMe/SATA partition with a physical or confirmed below-process reset | The strongest in-repository path toward E3, after review | Untested firmware revisions or mount changes |

The current in-repository E3 harness (`run-e3-block-reset.sh`) still provisions a disposable ext4
loopback or APFS sparsebundle, so label `guest_host_boundary` honestly. Certifying "production
NVMe" requires the Store data on that device (a dedicated disk or partition), not merely a sparse
file sitting on it, plus a reset confirmed below the process boundary, ideally a physical power cut.

## Prerequisites

1. A clean git worktree at the commit under test.
2. A built tree containing at least:
   - `glifistore_tests`
   - `glifistore_allocation_fault_tests`
   - `glifistore_crash_persistence`
   - the daemon crash binaries the E2 collector requires
3. Linux: root or `sudo` for loop, mount, fsck and optional `dm-flakey` (`e2fsprogs`, `lvm2`).
4. macOS: the ability to run `hdiutil` attach and detach and `diskutil`, plus non-interactive `sudo`
   for the offline `fsck_apfs -n` check in unattended runs; otherwise cases are `INCONCLUSIVE`.
5. A disposable target: no unrelated data on the image or device under reset.

Suggested builds:

```bash
cmake --preset unix-debug    # Linux
cmake --build --preset unix-debug --target \
  glifistore_tests glifistore_allocation_fault_tests \
  glifistore_crash_persistence glifistore_crash_daemon glifistored

cmake --preset macos-debug   # macOS
cmake --build --preset macos-debug --target \
  glifistore_tests glifistore_allocation_fault_tests \
  glifistore_crash_persistence glifistore_crash_daemon glifistored
```

## Pin the row

Before any E3 arming, record a stable, redacted identity. Never store serials, MACs or credentials.

| Field | Examples | Notes |
| --- | --- | --- |
| `pin_label` | `lab-nvme0-ext4-a`, `mac-mini-apfs-1` | Stable name for the release notes |
| `hardware_class` | `hosted-ci`, `diskimage-rehearsal`, `vm-dedicated-disk`, `bare-metal-nvme`, `bare-metal-other` | Drives the promotion eligibility hint |
| `fs_pin` | `ext4` or `apfs` | Must match the harness platform |
| `guest_host_boundary` | `loopback-image-on-host-nvme`, `hdiutil-sparsebundle-apfs`, `vm-virtio-blk-ext4-on-nvme`, `physical-nvme-partition-ext4` | Honesty gate |
| Mount and mkfs | `defaults`, `barrier=1`, APFS volume role | Attach redacted `findmnt` or `diskutil` output under `stages/pin-attachments/` |
| Cache and barrier | virtio `cache=none` or `writeback`; controller write cache on or off | Required for promotion review |
| Reset mechanism | `abrupt-detach`, `dm-flakey` with the exact fault mode, physical PDU cut | "Command issued" is not "power removed"; retain the stop and reset confirmation |

## Stages: E0 → E1 → E2 → E3

The orchestrator runs the stages in order and stops further heavy stages after a hard FAIL:

| Stage | Mechanism | Default command inside the orchestrator |
| --- | --- | --- |
| E0 | Unit, codec and reopen tests | `ctest -R '^glifistore_tests$'` |
| E1 | Allocation fault injection | `ctest -R '^glifistore_allocation_fault_tests$'` |
| E2 | Process-kill collector | `collect-durability-evidence.sh --run process-kill` |
| E3 | Block-reset harness | `run-e3-block-reset.sh --profile campaign` |

Do not skip stages on a promotion candidate. `--skip-e*` is diagnostic only and forces
`promotion_candidate=no`.

## Repetition policy

| Layer | Minimum for a campaign-prep tarball | Minimum before a maintainer may consider E3 promotion |
| --- | --- | --- |
| E2 process-kill suite | `--e2-repeat 3` (orchestrator default) | ≥ 3 clean passes on the pinned probe path |
| E3 per checkpoint | `--repeat 10` (orchestrator default) | ≥ 10 with zero unexplained outcomes for the claimed scope |
| E3 checkpoint set | Harness `campaign` profile (put smoke plus bootstrap, rotate and compaction) | A narrow put scope may use the smoke set only if the claim text says so; a general durable claim needs the full matrix scope |

Raise `--repeat` when timing jitter or flaky arming appears. Never discard FAIL or INCONCLUSIVE
rows.

## Outcomes

Each E3 case (checkpoint × repetition) uses the matrix definitions:

| Outcome | Meaning |
| --- | --- |
| PASS | Worker stop and reset confirmed below the process boundary; remount and non-repairing fsck; the recovery oracle accepts |
| FAIL | Reset confirmed, but the oracle disagrees, or committed state that must survive is lost or corrupt |
| INCONCLUSIVE | Checkpoint not reached, reset not confirmed, host or harness crash, lost console, fsck or remount infrastructure failure |

The campaign as a whole reports:

| Result | When |
| --- | --- |
| `passed` | E0–E3 all ran and passed (E3 `harness_result=passed`) |
| `failed` | Any hard FAIL |
| `inconclusive` | No FAIL, but E3 reported inconclusive |
| `incomplete` | Stages were skipped |

`passed` still leaves `e3_certified=no`.

## Run the campaign

### Linux ext4 (loopback rehearsal on a lab host)

```bash
sudo -v   # harness needs privileged loop/mount/fsck
./scripts/run-e3-campaign.sh \
  --output "$HOME/glifi-evidence/e3-campaign-$(date -u +%Y%m%dT%H%M%SZ)" \
  --build-dir build/unix-debug \
  --pin-label lab-host-ext4-loop-a \
  --hardware-class diskimage-rehearsal \
  --fs-pin ext4 \
  --guest-host-boundary loopback-image-on-host-filesystem \
  --platform linux-ext4 \
  --reset-mechanism dm-flakey \
  --dm-fault-mode drop-writes \
  --probe-path "$PWD" \
  --repeat 10 \
  --e2-repeat 3
```

### macOS APFS (diskimage rehearsal)

```bash
./scripts/run-e3-campaign.sh \
  --output "$HOME/glifi-evidence/e3-campaign-$(date -u +%Y%m%dT%H%M%SZ)" \
  --build-dir build/macos-debug \
  --pin-label lab-mac-apfs-image-a \
  --hardware-class diskimage-rehearsal \
  --fs-pin apfs \
  --guest-host-boundary hdiutil-sparsebundle-apfs \
  --platform macos-apfs \
  --reset-mechanism abrupt-detach \
  --probe-path "$PWD" \
  --repeat 10 \
  --e2-repeat 3
```

### Pin for a future dedicated-disk campaign

When the Store data and the reset target are a dedicated VM disk or bare-metal partition, not only a
loop file, set for example:

```bash
  --hardware-class vm-dedicated-disk \
  --guest-host-boundary vm-virtio-blk-ext4-on-nvme \
  --mount-options-note 'ext4 defaults,barrier=1; mkfs.ext4 -F' \
  --cache-barrier-note 'virtio cache=none; guest writeback off'
```

If the harness still uses a loop image on that disk, keep `loopback-…` in the boundary string. Never
claim bare-partition certification from a loop row.

### Options

| Flag | Default | Notes |
| --- | --- | --- |
| `--e3-profile` | `campaign` | Use `smoke` only for CI or a diagnostic rehearsal of the orchestrator path |
| `--repeat` | `10` | Per-checkpoint E3 repetitions; local bound 1..1000, hosted dispatch bound 1..100 |
| `--e2-repeat` | `3` | Process-kill suite repetitions; local bound 1..1000 |
| `--dm-fault-mode` | `drop-writes` | Linux only: `drop-writes`, `error-writes` or `all-io-error`; the artifact records the selected mode |

## CI rehearsal

`.github/workflows/durability-evidence.yml` rehearses; it never certifies:

| Trigger | What runs | Claim |
| --- | --- | --- |
| PR or push to `main` | E2 metadata and the linux-ext4 smoke harness | Harness regression only |
| Weekly schedule | E2 process-kill; a 256-case reproducible randomized crash/reopen campaign; the linux-ext4 campaign profile once for each bounded dm-flakey mode; the macOS APFS campaign (repeat 2); the hosted-ci E0→E3 orchestrator (`--e3-profile smoke`, low repetitions) | Still `e3_certified=no`; `hardware_class=hosted-ci` cannot promote |
| `workflow_dispatch` | Operator-chosen E2/E3 profile, repeat and Linux dm-flakey mode; optional orchestrator | The same honesty gates |

After every harness or campaign write, the scripts and CI call
`scripts/assert-e3-rehearsal-honesty.sh`. It checks the SHA-256 manifest and rejects a harness PASS
row that lacks paused-worker, reset, filesystem-check or recovery confirmation.

## Artifact layout

```text
<output>/
  campaign-provenance.txt      # always e3_certified=no
  campaign-pin.txt
  campaign-summary.md
  promotion-checklist.md       # all boxes unchecked at creation
  stage-results.tsv
  commands.txt
  manifest.sha256
  e2-orchestrator.log
  e3-orchestrator.log
  stages/
    pin-attachments/           # operator drops redacted findmnt/diskutil here
    e0/                        # ctest log + result
    e1/
    e2/                        # collect-durability-evidence artifact
    e3/                        # run-e3-block-reset artifact (provenance, results.tsv, …)
<output>.tar.gz
<output>.tar.gz.sha256
```

Retain the tarball and its `.sha256` beside the release evidence. Re-verify them with
`sha256sum -c` or `shasum -a 256 -c` before review.

## Maintainer sign-off checklist

Every artifact carries this checklist as `promotion-checklist.md`. Promotion is human-only:

- [ ] Clean source commit matches `campaign-provenance.txt`
- [ ] Pin identity complete (hardware/VM, FS, mkfs/mount, cache/barrier, boundary, reset)
- [ ] E0, E1, and E2 stages passed on this same pin (not skipped)
- [ ] E3 campaign profile covered required checkpoints; repeat ≥ policy minimum
- [ ] Zero FAIL and zero unexplained INCONCLUSIVE outcomes
- [ ] Every retained PASS case has `worker_stop_confirmed=yes` and `reset_confirmed=yes`
- [ ] Exact dm fault mode / reset mechanism and the absence or presence of a block-level trace
  reviewed
- [ ] SHA-256 manifest verifies; tarball retained
- [ ] Row is not hosted-CI / disposable-image-only if claiming production NVMe/SATA
- [ ] Release maintainer recorded artifact reference in release notes
- [ ] Matrix row in `platform-durability-evidence.md` updated only after the above

## Promote a row

1. The campaign tarball result is `passed`, with `source_dirty=no` and complete stages.
2. `hardware_class` and `guest_host_boundary` match the claim you intend to publish.
3. A release or storage maintainer has checked every item of the checklist above.
4. Edit the Current evidence table in `docs/architecture/platform-durability-evidence.md`, for that
   row only.
5. Reference the tarball SHA-256 and the pin label in the release notes.
6. Never patch the harness scripts to mark an artifact as E3 certified in place of steps 4–5.

Until steps 1–5 succeed, leave the matrix row not E3 certified and keep the artifacts labeled
`e3_certified=no`.

## Related

- [Platform durability evidence](../architecture/platform-durability-evidence.md)
- [Test strategy](../development/test-strategy.md)
- [Operations](README.md)
- `.github/workflows/durability-evidence.yml`: CI rehearsal only, not a promotion path
