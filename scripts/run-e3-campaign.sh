#!/usr/bin/env bash
# Operator-facing E3 campaign orchestrator.
#
# Sequences E0 → E1 → E2 → E3 on a declared pin, aggregates evidence, writes a
# SHA-256 manifest, and packs a tarball. Artifacts ALWAYS keep e3_certified=no.
# A maintainer promotes only after the gates in
# docs/architecture/platform-durability-evidence.md and
# docs/operations/e3-campaign.md are met and recorded.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir=""
build_dir=""
platform="auto"
reset_mechanism="auto"
dm_fault_mode="drop-writes"
dm_fault_mode_explicit="no"
e3_profile="campaign"
repeat=10
e2_repeat=3
image_size_mib=""
work_root=""
keep_work="no"
probe_path=""
pin_label=""
hardware_class=""
fs_pin=""
guest_host_boundary=""
mount_options_note=""
cache_barrier_note=""
skip_e0="no"
skip_e1="no"
skip_e2="no"
skip_e3="no"
skip_tarball="no"

usage() {
  cat <<'EOF'
Usage:
  scripts/run-e3-campaign.sh --output DIR --build-dir DIR \
    --pin-label LABEL --hardware-class CLASS --fs-pin FS \
    --guest-host-boundary BOUNDARY [options]

Required pin declaration (recorded into campaign-pin.txt):
  --pin-label LABEL              Stable redacted row name (no serials/credentials)
  --hardware-class CLASS         hosted-ci | diskimage-rehearsal | vm-dedicated-disk |
                                 bare-metal-nvme | bare-metal-other
  --fs-pin FS                    ext4 | apfs
  --guest-host-boundary BOUNDARY Honest boundary string, e.g.
                                 loopback-image-on-host-nvme |
                                 hdiutil-sparsebundle-apfs |
                                 vm-virtio-blk-ext4-on-nvme |
                                 physical-nvme-partition-ext4

Options:
  --platform auto|linux-ext4|macos-apfs   E3 harness row (default: auto)
  --e3-profile smoke|campaign    Checkpoint set for E3 (default: campaign)
  --reset-mechanism auto|abrupt-detach|dm-flakey
  --dm-fault-mode drop-writes|error-writes|all-io-error
                                 Linux dm-flakey behavior (default: drop-writes)
  --repeat N                     E3 per-checkpoint repetitions, 1..1000 (default: 10)
  --e2-repeat N                  E2 process-kill suite repetitions, 1..1000 (default: 3)
  --probe-path PATH              Writable path on the FS under E2 test (default: repo)
  --image-size-mib N             Passed through to run-e3-block-reset.sh
  --work-root DIR                Scratch parent for the E3 harness
  --keep-work                    Retain E3 image/mount scratch
  --mount-options-note TEXT      Free-form mount/mkfs note for the pin file
  --cache-barrier-note TEXT      Free-form cache/barrier / virtio cache policy note
  --skip-e0 | --skip-e1 | --skip-e2 | --skip-e3
                                 Skip a stage (diagnostic only; blocks promotion)
  --skip-tarball                 Do not write DIR.tar.gz
  -h, --help

This script never sets e3_certified=yes. Hosted CI and loopback/diskimage rows remain
campaign-prep / rehearsal evidence. See docs/operations/e3-campaign.md.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      [[ $# -ge 2 ]] || { echo "error: --output requires a directory" >&2; exit 2; }
      output_dir="$2"
      shift 2
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "error: --build-dir requires a directory" >&2; exit 2; }
      build_dir="$2"
      shift 2
      ;;
    --platform)
      [[ $# -ge 2 ]] || { echo "error: --platform requires a value" >&2; exit 2; }
      platform="$2"
      shift 2
      ;;
    --e3-profile)
      [[ $# -ge 2 ]] || { echo "error: --e3-profile requires a value" >&2; exit 2; }
      e3_profile="$2"
      shift 2
      ;;
    --reset-mechanism)
      [[ $# -ge 2 ]] || { echo "error: --reset-mechanism requires a value" >&2; exit 2; }
      reset_mechanism="$2"
      shift 2
      ;;
    --dm-fault-mode)
      [[ $# -ge 2 ]] || { echo "error: --dm-fault-mode requires a value" >&2; exit 2; }
      dm_fault_mode="$2"
      dm_fault_mode_explicit="yes"
      shift 2
      ;;
    --repeat)
      [[ $# -ge 2 ]] || { echo "error: --repeat requires a count" >&2; exit 2; }
      repeat="$2"
      shift 2
      ;;
    --e2-repeat)
      [[ $# -ge 2 ]] || { echo "error: --e2-repeat requires a count" >&2; exit 2; }
      e2_repeat="$2"
      shift 2
      ;;
    --probe-path)
      [[ $# -ge 2 ]] || { echo "error: --probe-path requires a path" >&2; exit 2; }
      probe_path="$2"
      shift 2
      ;;
    --image-size-mib)
      [[ $# -ge 2 ]] || { echo "error: --image-size-mib requires a count" >&2; exit 2; }
      image_size_mib="$2"
      shift 2
      ;;
    --work-root)
      [[ $# -ge 2 ]] || { echo "error: --work-root requires a directory" >&2; exit 2; }
      work_root="$2"
      shift 2
      ;;
    --keep-work)
      keep_work="yes"
      shift
      ;;
    --pin-label)
      [[ $# -ge 2 ]] || { echo "error: --pin-label requires a value" >&2; exit 2; }
      pin_label="$2"
      shift 2
      ;;
    --hardware-class)
      [[ $# -ge 2 ]] || { echo "error: --hardware-class requires a value" >&2; exit 2; }
      hardware_class="$2"
      shift 2
      ;;
    --fs-pin)
      [[ $# -ge 2 ]] || { echo "error: --fs-pin requires a value" >&2; exit 2; }
      fs_pin="$2"
      shift 2
      ;;
    --guest-host-boundary)
      [[ $# -ge 2 ]] || { echo "error: --guest-host-boundary requires a value" >&2; exit 2; }
      guest_host_boundary="$2"
      shift 2
      ;;
    --mount-options-note)
      [[ $# -ge 2 ]] || { echo "error: --mount-options-note requires a value" >&2; exit 2; }
      mount_options_note="$2"
      shift 2
      ;;
    --cache-barrier-note)
      [[ $# -ge 2 ]] || { echo "error: --cache-barrier-note requires a value" >&2; exit 2; }
      cache_barrier_note="$2"
      shift 2
      ;;
    --skip-e0) skip_e0="yes"; shift ;;
    --skip-e1) skip_e1="yes"; shift ;;
    --skip-e2) skip_e2="yes"; shift ;;
    --skip-e3) skip_e3="yes"; shift ;;
    --skip-tarball) skip_tarball="yes"; shift ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$output_dir" ]] || { echo "error: --output is required" >&2; exit 2; }
[[ -n "$build_dir" ]] || { echo "error: --build-dir is required" >&2; exit 2; }
[[ -n "$pin_label" ]] || { echo "error: --pin-label is required" >&2; exit 2; }
[[ -n "$hardware_class" ]] || { echo "error: --hardware-class is required" >&2; exit 2; }
[[ -n "$fs_pin" ]] || { echo "error: --fs-pin is required" >&2; exit 2; }
[[ -n "$guest_host_boundary" ]] || { echo "error: --guest-host-boundary is required" >&2; exit 2; }

[[ "$e3_profile" == "smoke" || "$e3_profile" == "campaign" ]] ||
  { echo "error: --e3-profile accepts smoke|campaign" >&2; exit 2; }

case "$hardware_class" in
  hosted-ci|diskimage-rehearsal|vm-dedicated-disk|bare-metal-nvme|bare-metal-other) ;;
  *)
    echo "error: --hardware-class must be hosted-ci|diskimage-rehearsal|vm-dedicated-disk|bare-metal-nvme|bare-metal-other" >&2
    exit 2
    ;;
esac
case "$fs_pin" in
  ext4|apfs) ;;
  *)
    echo "error: --fs-pin must be ext4|apfs" >&2
    exit 2
    ;;
esac

[[ -d "$build_dir" ]] || { echo "error: build directory does not exist: $build_dir" >&2; exit 2; }
build_dir="$(cd "$build_dir" && pwd)"
[[ "$repeat" =~ ^[1-9][0-9]*$ && "$repeat" -le 1000 ]] ||
  { echo "error: --repeat must be in 1..1000" >&2; exit 2; }
[[ "$e2_repeat" =~ ^[1-9][0-9]*$ && "$e2_repeat" -le 1000 ]] ||
  { echo "error: --e2-repeat must be in 1..1000" >&2; exit 2; }
[[ "$platform" == "auto" || "$platform" == "linux-ext4" || "$platform" == "macos-apfs" ]] ||
  { echo "error: --platform accepts auto|linux-ext4|macos-apfs" >&2; exit 2; }
[[ "$reset_mechanism" == "auto" || "$reset_mechanism" == "abrupt-detach" || "$reset_mechanism" == "dm-flakey" ]] ||
  { echo "error: --reset-mechanism accepts auto|abrupt-detach|dm-flakey" >&2; exit 2; }
[[ "$dm_fault_mode" == "drop-writes" || "$dm_fault_mode" == "error-writes" || "$dm_fault_mode" == "all-io-error" ]] ||
  { echo "error: --dm-fault-mode accepts drop-writes|error-writes|all-io-error" >&2; exit 2; }

if [[ -e "$output_dir" ]]; then
  echo "error: output path already exists; refusing to overwrite: $output_dir" >&2
  exit 2
fi

if [[ -z "$probe_path" ]]; then
  probe_path="$root"
fi
[[ -d "$probe_path" ]] || { echo "error: probe path must be a directory: $probe_path" >&2; exit 2; }
probe_path="$(cd "$probe_path" && pwd)"

os_name="$(uname -s)"
if [[ "$platform" == "auto" ]]; then
  case "$os_name" in
    Linux) platform="linux-ext4" ;;
    Darwin) platform="macos-apfs" ;;
    *)
      echo "error: unsupported host OS for auto platform selection: $os_name" >&2
      exit 2
      ;;
  esac
fi

# Honest FS pin vs harness platform check.
if [[ "$platform" == "linux-ext4" && "$fs_pin" != "ext4" ]]; then
  echo "error: linux-ext4 platform requires --fs-pin ext4" >&2
  exit 2
fi
if [[ "$platform" == "macos-apfs" && "$fs_pin" != "apfs" ]]; then
  echo "error: macos-apfs platform requires --fs-pin apfs" >&2
  exit 2
fi
if [[ "$dm_fault_mode_explicit" == "yes" ]] &&
   { [[ "$platform" != "linux-ext4" ]] || [[ "$reset_mechanism" == "abrupt-detach" ]]; }; then
  echo "error: --dm-fault-mode requires a Linux dm-flakey row" >&2
  exit 2
fi
if [[ "$platform" != "linux-ext4" || "$reset_mechanism" == "abrupt-detach" ]]; then
  dm_fault_mode="not-applicable"
fi

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1"
  else
    return 1
  fi
}

utc_now="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
source_commit="$(git -C "$root" rev-parse HEAD 2>/dev/null || printf 'unknown\n')"
source_dirty="no"
if ! git -C "$root" diff --quiet --ignore-submodules HEAD 2>/dev/null ||
   ! git -C "$root" diff --quiet --ignore-submodules --cached HEAD 2>/dev/null ||
   [[ -n "$(git -C "$root" ls-files --others --exclude-standard 2>/dev/null)" ]]; then
  source_dirty="yes"
fi

mkdir -p "$output_dir/stages" || exit 2
output_dir="$(cd "$output_dir" && pwd)"
stages="$output_dir/stages"
commands="$output_dir/commands.txt"
pin_file="$output_dir/campaign-pin.txt"
provenance="$output_dir/campaign-provenance.txt"
summary="$output_dir/campaign-summary.md"
checklist="$output_dir/promotion-checklist.md"
stage_results="$output_dir/stage-results.tsv"
: >"$commands"
printf 'stage\tresult\tnotes\n' >"$stage_results"

record_command() {
  printf '%s\n' "$*" >>"$commands"
}

run_logged() {
  local log_file="$1"
  shift
  record_command "$*"
  "$@" >"$log_file" 2>&1
}

# Promotion eligibility hint only — never flips e3_certified.
promotion_candidate="no"
promotion_blocker="campaign-prep; e3_certified remains no until human review"
case "$hardware_class" in
  hosted-ci|diskimage-rehearsal)
    promotion_blocker="hardware_class=$hardware_class cannot certify production NVMe/SATA; rehearsal only"
    ;;
  vm-dedicated-disk|bare-metal-nvme|bare-metal-other)
    if [[ "$guest_host_boundary" == *loopback* || "$guest_host_boundary" == *sparsebundle* || "$guest_host_boundary" == *diskimage* ]]; then
      promotion_blocker="guest_host_boundary still names a disposable image row; certifies that image row only, not bare NVMe firmware"
    else
      promotion_candidate="review-required"
      promotion_blocker="pin may be promotion-reviewable after PASS with zero unexplained outcomes; still e3_certified=no until maintainer sign-off"
    fi
    ;;
esac
if [[ "$source_dirty" == "yes" ]]; then
  promotion_candidate="no"
  promotion_blocker="dirty source worktree; artifact is diagnostic only"
fi
if [[ "$skip_e0" == "yes" || "$skip_e1" == "yes" || "$skip_e2" == "yes" || "$skip_e3" == "yes" ]]; then
  promotion_candidate="no"
  promotion_blocker="one or more stages skipped; not promotion-eligible"
fi

{
  printf 'schema=glifistore-durability-e3-campaign-v2\n'
  printf 'generated_utc=%s\n' "$utc_now"
  printf 'orchestrator=scripts/run-e3-campaign.sh\n'
  printf 'pin_label=%s\n' "$pin_label"
  printf 'hardware_class=%s\n' "$hardware_class"
  printf 'fs_pin=%s\n' "$fs_pin"
  printf 'guest_host_boundary=%s\n' "$guest_host_boundary"
  printf 'platform_row=%s\n' "$platform"
  printf 'e3_profile=%s\n' "$e3_profile"
  printf 'e3_repeat=%s\n' "$repeat"
  printf 'e2_repeat=%s\n' "$e2_repeat"
  printf 'reset_mechanism=%s\n' "$reset_mechanism"
  printf 'dm_fault_mode_request=%s\n' "$dm_fault_mode"
  printf 'source_commit=%s\n' "$source_commit"
  printf 'source_dirty=%s\n' "$source_dirty"
  printf 'repository=%s\n' "$root"
  printf 'build_directory=%s\n' "$build_dir"
  printf 'probe_path=%s\n' "$probe_path"
  printf 'physical_power_cut=no\n'
  printf 'e3_certified=no\n'
  printf 'e4_certified=no\n'
  printf 'promotion_candidate=%s\n' "$promotion_candidate"
  printf 'promotion_blocker=%s\n' "$promotion_blocker"
  printf 'maximum_possible_label=E3-campaign-prep\n'
} >"$provenance"

{
  printf '# GlifiStore E3 campaign pin declaration\n\n'
  printf 'Fill only stable redacted labels. Do not store serial numbers, MACs, or credentials.\n\n'
  printf -- '- pin_label: `%s`\n' "$pin_label"
  printf -- '- hardware_class: `%s`\n' "$hardware_class"
  printf -- '- fs_pin: `%s`\n' "$fs_pin"
  printf -- '- guest_host_boundary: `%s`\n' "$guest_host_boundary"
  printf -- '- platform_row (harness): `%s`\n' "$platform"
  printf -- '- reset_mechanism: `%s`\n' "$reset_mechanism"
  printf -- '- dm_fault_mode_request: `%s`\n' "$dm_fault_mode"
  printf -- '- mount_options_note: `%s`\n' "${mount_options_note:-<unset>}"
  printf -- '- cache_barrier_note: `%s`\n' "${cache_barrier_note:-<unset>}"
  printf -- '- host_uname: `%s`\n' "$(uname -a)"
  printf -- '- generated_utc: `%s`\n' "$utc_now"
  printf -- '- source_commit: `%s`\n' "$source_commit"
  printf -- '- source_dirty: `%s`\n' "$source_dirty"
  printf '\nOperator: attach `findmnt` / `mount` / `diskutil info` excerpts (redacted) under\n'
  printf '`stages/pin-attachments/` if promoting beyond rehearsal.\n'
} >"$pin_file"

mkdir -p "$stages/pin-attachments"

campaign_failed=0
campaign_inconclusive=0

# --- E0 ---
e0_dir="$stages/e0"
if [[ "$skip_e0" == "yes" ]]; then
  mkdir -p "$e0_dir"
  printf 'skipped\n' >"$e0_dir/result.txt"
  printf 'e0\tskipped\toperator requested --skip-e0\n' >>"$stage_results"
else
  mkdir -p "$e0_dir"
  printf 'Running E0 (glifistore_tests)...\n'
  if run_logged "$e0_dir/ctest.log" ctest --test-dir "$build_dir" -R '^glifistore_tests$' --output-on-failure; then
    printf 'passed\n' >"$e0_dir/result.txt"
    printf 'e0\tpassed\tglifistore_tests\n' >>"$stage_results"
  else
    printf 'failed\n' >"$e0_dir/result.txt"
    printf 'e0\tfailed\tglifistore_tests\n' >>"$stage_results"
    campaign_failed=1
  fi
fi

# --- E1 ---
e1_dir="$stages/e1"
if [[ "$skip_e1" == "yes" ]]; then
  mkdir -p "$e1_dir"
  printf 'skipped\n' >"$e1_dir/result.txt"
  printf 'e1\tskipped\toperator requested --skip-e1\n' >>"$stage_results"
elif [[ "$campaign_failed" -ne 0 ]]; then
  mkdir -p "$e1_dir"
  printf 'skipped\n' >"$e1_dir/result.txt"
  printf 'e1\tskipped\tprior stage failed\n' >>"$stage_results"
else
  mkdir -p "$e1_dir"
  printf 'Running E1 (glifistore_allocation_fault_tests)...\n'
  if run_logged "$e1_dir/ctest.log" ctest --test-dir "$build_dir" -R '^glifistore_allocation_fault_tests$' --output-on-failure; then
    printf 'passed\n' >"$e1_dir/result.txt"
    printf 'e1\tpassed\tglifistore_allocation_fault_tests\n' >>"$stage_results"
  else
    printf 'failed\n' >"$e1_dir/result.txt"
    printf 'e1\tfailed\tglifistore_allocation_fault_tests\n' >>"$stage_results"
    campaign_failed=1
  fi
fi

# --- E2 ---
e2_dir="$stages/e2"
if [[ "$skip_e2" == "yes" ]]; then
  mkdir -p "$e2_dir"
  printf 'skipped\n' >"$e2_dir/result.txt"
  printf 'e2\tskipped\toperator requested --skip-e2\n' >>"$stage_results"
elif [[ "$campaign_failed" -ne 0 ]]; then
  mkdir -p "$e2_dir"
  printf 'skipped\n' >"$e2_dir/result.txt"
  printf 'e2\tskipped\tprior stage failed\n' >>"$stage_results"
else
  printf 'Running E2 (collect-durability-evidence process-kill, repeat=%s)...\n' "$e2_repeat"
  e2_cmd=(
    "$root/scripts/collect-durability-evidence.sh"
    --output "$e2_dir"
    --build-dir "$build_dir"
    --probe-path "$probe_path"
    --run process-kill
    --repeat "$e2_repeat"
  )
  record_command "${e2_cmd[*]}"
  if "${e2_cmd[@]}" >"$output_dir/e2-orchestrator.log" 2>&1; then
    printf 'passed\n' >"$stages/e2-result.txt"
    printf 'e2\tpassed\tprocess-kill repeat=%s\n' "$e2_repeat" >>"$stage_results"
  else
    mkdir -p "$e2_dir"
    printf 'failed\n' >"$stages/e2-result.txt"
    printf 'e2\tfailed\tsee e2-orchestrator.log\n' >>"$stage_results"
    campaign_failed=1
  fi
fi

# --- E3 ---
e3_dir="$stages/e3"
if [[ "$skip_e3" == "yes" ]]; then
  mkdir -p "$e3_dir"
  printf 'skipped\n' >"$e3_dir/result.txt"
  printf 'e3\tskipped\toperator requested --skip-e3\n' >>"$stage_results"
elif [[ "$campaign_failed" -ne 0 ]]; then
  mkdir -p "$e3_dir"
  printf 'skipped\n' >"$e3_dir/result.txt"
  printf 'e3\tskipped\tprior stage failed\n' >>"$stage_results"
else
  printf 'Running E3 block-reset (profile=%s, repeat=%s)...\n' "$e3_profile" "$repeat"
  e3_cmd=(
    "$root/scripts/run-e3-block-reset.sh"
    --output "$e3_dir"
    --build-dir "$build_dir"
    --platform "$platform"
    --profile "$e3_profile"
    --reset-mechanism "$reset_mechanism"
    --repeat "$repeat"
  )
  if [[ "$dm_fault_mode" != "not-applicable" && "$dm_fault_mode_explicit" == "yes" ]]; then
    e3_cmd+=(--dm-fault-mode "$dm_fault_mode")
  fi
  if [[ -n "$image_size_mib" ]]; then
    e3_cmd+=(--image-size-mib "$image_size_mib")
  fi
  if [[ -n "$work_root" ]]; then
    e3_cmd+=(--work-root "$work_root")
  fi
  if [[ "$keep_work" == "yes" ]]; then
    e3_cmd+=(--keep-work)
  fi
  record_command "${e3_cmd[*]}"
  "${e3_cmd[@]}" >"$output_dir/e3-orchestrator.log" 2>&1
  e3_rc=$?
  if [[ "$e3_rc" -eq 0 ]]; then
    printf 'passed\n' >"$stages/e3-result.txt"
    printf 'e3\tpassed\tprofile=%s repeat=%s\n' "$e3_profile" "$repeat" >>"$stage_results"
  else
    # Distinguish FAIL vs INCONCLUSIVE from harness provenance when present.
    e3_outcome="failed"
    if [[ -f "$e3_dir/provenance.txt" ]] && grep -q 'harness_result=inconclusive' "$e3_dir/provenance.txt"; then
      e3_outcome="inconclusive"
      campaign_inconclusive=1
    else
      campaign_failed=1
    fi
    printf '%s\n' "$e3_outcome" >"$stages/e3-result.txt"
    printf 'e3\t%s\tsee stages/e3/ and e3-orchestrator.log\n' "$e3_outcome" >>"$stage_results"
  fi
  # Enforce honesty: child harness must keep e3_certified=no.
  if [[ -f "$e3_dir/provenance.txt" ]] && ! grep -q 'e3_certified=no' "$e3_dir/provenance.txt"; then
    echo "error: E3 harness artifact missing e3_certified=no" >&2
    campaign_failed=1
  fi
fi

campaign_result="passed"
if [[ "$campaign_failed" -ne 0 ]]; then
  campaign_result="failed"
elif [[ "$campaign_inconclusive" -ne 0 ]]; then
  campaign_result="inconclusive"
elif [[ "$skip_e0" == "yes" || "$skip_e1" == "yes" || "$skip_e2" == "yes" || "$skip_e3" == "yes" ]]; then
  campaign_result="incomplete"
fi

{
  printf '\n[campaign_totals]\n'
  printf 'campaign_result=%s\n' "$campaign_result"
  printf 'e3_certified=no\n'
  printf 'e4_certified=no\n'
  printf 'promotion_candidate=%s\n' "$promotion_candidate"
  printf 'finished_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} >>"$provenance"

{
  printf '# GlifiStore E3 campaign artifact\n\n'
  printf -- '- Generated (UTC): `%s`\n' "$utc_now"
  printf -- '- Source commit: `%s`\n' "$source_commit"
  printf -- '- Source worktree dirty: `%s`\n' "$source_dirty"
  printf -- '- Pin label: `%s`\n' "$pin_label"
  printf -- '- Hardware class: `%s`\n' "$hardware_class"
  printf -- '- FS pin: `%s`\n' "$fs_pin"
  printf -- '- Guest/host boundary: `%s`\n' "$guest_host_boundary"
  printf -- '- Platform row: `%s`\n' "$platform"
  printf -- '- E3 profile / repeat: `%s` / `%s`\n' "$e3_profile" "$repeat"
  printf -- '- E2 repeat: `%s`\n' "$e2_repeat"
  printf -- '- Campaign result: `%s`\n' "$campaign_result"
  printf -- '- E3 certified: `no`\n'
  printf -- '- E4 certified: `no`\n'
  printf -- '- Promotion candidate: `%s`\n' "$promotion_candidate"
  printf -- '- Promotion blocker: %s\n\n' "$promotion_blocker"
  printf 'This tarball is **campaign-prep**. It does not certify sudden power loss.\n'
  printf 'Promotion procedure: `docs/operations/e3-campaign.md` and\n'
  printf '`docs/architecture/platform-durability-evidence.md`.\n'
} >"$summary"

{
  printf '# Maintainer promotion checklist (human only)\n\n'
  printf 'All boxes start unchecked. Do **not** flip `e3_certified` in scripts.\n'
  printf 'Promotion is a release-notes / matrix edit by a maintainer after review.\n\n'
  printf -- '- [ ] Clean source commit matches `campaign-provenance.txt`\n'
  printf -- '- [ ] Pin identity complete (hardware/VM, FS, mkfs/mount, cache/barrier, boundary, reset)\n'
  printf -- '- [ ] E0, E1, and E2 stages passed on this same pin (not skipped)\n'
  printf -- '- [ ] E3 campaign profile covered required checkpoints; repeat ≥ policy minimum\n'
  printf -- '- [ ] Zero FAIL and zero unexplained INCONCLUSIVE outcomes (or each INCONCLUSIVE explained and accepted)\n'
  printf -- '- [ ] Every retained PASS case has `worker_stop_confirmed=yes` and `reset_confirmed=yes`\n'
  printf -- '- [ ] Exact dm fault mode / reset mechanism and any block-trace limitation were reviewed\n'
  printf -- '- [ ] SHA-256 manifest verifies; tarball retained with release evidence store\n'
  printf -- '- [ ] Row is not hosted-CI / disposable-image-only if claiming production NVMe/SATA\n'
  printf -- '- [ ] Release maintainer recorded artifact reference in release notes\n'
  printf -- '- [ ] Matrix row in `platform-durability-evidence.md` updated only after the above\n'
  printf '\nUntil every box is checked by a human, keep **E3 certified: no**.\n'
} >"$checklist"

tarball_path=""
if [[ "$skip_tarball" != "yes" ]]; then
  parent="$(dirname "$output_dir")"
  base="$(basename "$output_dir")"
  tarball_path="$parent/${base}.tar.gz"
  if [[ -e "$tarball_path" ]]; then
    echo "error: tarball already exists; refusing to overwrite: $tarball_path" >&2
    exit 2
  fi
  # Record before sealing commands.txt into the manifest.
  record_command "tar -C $parent -czf $tarball_path $base"
fi

# Manifest over the immutable campaign tree (before tarball).
manifest="$output_dir/manifest.sha256"
: >"$manifest"
checksum_available="yes"
while IFS= read -r artifact; do
  [[ "$artifact" == "$manifest" ]] && continue
  if ! digest_line="$(sha256_file "$artifact" 2>/dev/null)"; then
    checksum_available="no"
    break
  fi
  digest="${digest_line%% *}"
  printf '%s  %s\n' "$digest" "${artifact#"$output_dir/"}" >>"$manifest"
done < <(find "$output_dir" -type f | LC_ALL=C sort)
if [[ "$checksum_available" == "no" ]]; then
  printf 'unavailable  no SHA-256 utility found\n' >"$manifest"
fi

if [[ -n "$tarball_path" ]]; then
  tar -C "$parent" -czf "$tarball_path" "$base" || exit 2
  if digest_line="$(sha256_file "$tarball_path" 2>/dev/null)"; then
    printf '%s\n' "$digest_line" >"${tarball_path}.sha256"
  fi
fi

if ! "$root/scripts/assert-e3-rehearsal-honesty.sh" --dir "$output_dir" --kind campaign; then
  echo "error: campaign honesty assert failed" >&2
  campaign_result="failed"
fi

printf 'E3 campaign artifact written to %s\n' "$output_dir"
if [[ -n "$tarball_path" ]]; then
  printf 'Evidence tarball: %s\n' "$tarball_path"
fi
printf 'Result: %s; e3_certified: no; promotion_candidate: %s\n' \
  "$campaign_result" "$promotion_candidate"

case "$campaign_result" in
  passed) exit 0 ;;
  inconclusive|incomplete) exit 1 ;;
  *) exit 1 ;;
esac
