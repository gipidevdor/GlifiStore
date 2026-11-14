#!/usr/bin/env bash
# Provision a disposable pinned filesystem row and exercise abrupt block-device
# reset (not process-kill alone). Records a campaign artifact with honest
# labeling: this never claims E3 certification unless every promotion gate in
# docs/architecture/platform-durability-evidence.md is satisfied and retained.
#
# Preferred first-row paths:
#   Linux  — sparse image + ext4 (+ optional dm-flakey)
#   macOS  — APFS disk image + hdiutil detach -force
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir=""
build_dir=""
platform="auto"
profile="smoke"
reset_mechanism="auto"
dm_fault_mode="drop-writes"
dm_fault_mode_explicit="no"
dm_fault_window_ms=250
image_size_mib=""
repeat=1
work_root=""
keep_work="no"
crash_bin=""

usage() {
  cat <<'EOF'
Usage:
  scripts/run-e3-block-reset.sh --output DIR --build-dir DIR [options]

Options:
  --platform auto|linux-ext4|macos-apfs   Storage row to provision (default: auto)
  --profile smoke|campaign               Checkpoint set (default: smoke)
  --reset-mechanism auto|abrupt-detach|dm-flakey
                                         auto picks dm-flakey on Linux when available
  --dm-fault-mode drop-writes|error-writes|all-io-error
                                         dm-flakey down-interval behavior (default: drop-writes)
  --image-size-mib N                     Sparse image size (smoke default 1024, campaign 2048)
  --repeat N                             Repeat each checkpoint case, 1..1000 (default: 1)
  --work-root DIR                        Parent for image/mount scratch (default: under output)
  --keep-work                            Retain image/mount scratch after the run
  -h, --help                             Show this help

PASS (harness): every armed reset is confirmed below the process boundary, remount/fsck
completes without silent repair that would discard evidence, and the recovery oracle for
that checkpoint accepts the result.

This script labels artifacts as E3-harness / campaign-prep. It does NOT set
e3_certified=yes. Hosted CI and loopback/diskimage rows are rehearsal evidence until a
reviewed pinned campaign artifact is published with the release.

For the operator E0→E2→E3 campaign wrapper (tarball + manifest, still e3_certified=no),
see scripts/run-e3-campaign.sh and docs/operations/e3-campaign.md.
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
    --profile)
      [[ $# -ge 2 ]] || { echo "error: --profile requires a value" >&2; exit 2; }
      profile="$2"
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
    --image-size-mib)
      [[ $# -ge 2 ]] || { echo "error: --image-size-mib requires a count" >&2; exit 2; }
      image_size_mib="$2"
      shift 2
      ;;
    --repeat)
      [[ $# -ge 2 ]] || { echo "error: --repeat requires a count" >&2; exit 2; }
      repeat="$2"
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
[[ -d "$build_dir" ]] || { echo "error: build directory does not exist: $build_dir" >&2; exit 2; }
build_dir="$(cd "$build_dir" && pwd)"
[[ "$profile" == "smoke" || "$profile" == "campaign" ]] ||
  { echo "error: --profile accepts smoke|campaign" >&2; exit 2; }
[[ "$platform" == "auto" || "$platform" == "linux-ext4" || "$platform" == "macos-apfs" ]] ||
  { echo "error: --platform accepts auto|linux-ext4|macos-apfs" >&2; exit 2; }
[[ "$reset_mechanism" == "auto" || "$reset_mechanism" == "abrupt-detach" || "$reset_mechanism" == "dm-flakey" ]] ||
  { echo "error: --reset-mechanism accepts auto|abrupt-detach|dm-flakey" >&2; exit 2; }
[[ "$dm_fault_mode" == "drop-writes" || "$dm_fault_mode" == "error-writes" || "$dm_fault_mode" == "all-io-error" ]] ||
  { echo "error: --dm-fault-mode accepts drop-writes|error-writes|all-io-error" >&2; exit 2; }
[[ "$repeat" =~ ^[1-9][0-9]*$ && "$repeat" -le 1000 ]] ||
  { echo "error: --repeat must be in 1..1000" >&2; exit 2; }
if [[ -e "$output_dir" ]]; then
  echo "error: output path already exists; refusing to overwrite: $output_dir" >&2
  exit 2
fi

crash_bin="$build_dir/glifistore_crash_persistence"
[[ -x "$crash_bin" ]] || {
  echo "error: missing executable $crash_bin (build glifistore_crash_persistence first)" >&2
  exit 2
}

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
if [[ "$platform" == "linux-ext4" && "$os_name" != "Linux" ]]; then
  echo "error: linux-ext4 requires a Linux host" >&2
  exit 2
fi
if [[ "$platform" == "macos-apfs" && "$os_name" != "Darwin" ]]; then
  echo "error: macos-apfs requires macOS" >&2
  exit 2
fi

if [[ -z "$image_size_mib" ]]; then
  if [[ "$profile" == "campaign" ]]; then
    image_size_mib=2048
  else
    # Must exceed default Store reserved_free_bytes (256 MiB) plus Segment preallocate.
    image_size_mib=1024
  fi
fi
[[ "$image_size_mib" =~ ^[1-9][0-9]*$ ]] || {
  echo "error: --image-size-mib must be a positive integer" >&2
  exit 2
}

if [[ "$reset_mechanism" == "auto" ]]; then
  if [[ "$platform" == "linux-ext4" ]] && command -v dmsetup >/dev/null 2>&1; then
    reset_mechanism="dm-flakey"
  else
    reset_mechanism="abrupt-detach"
  fi
fi
if [[ "$reset_mechanism" == "dm-flakey" && "$platform" != "linux-ext4" ]]; then
  echo "error: dm-flakey is only supported with linux-ext4" >&2
  exit 2
fi
if [[ "$reset_mechanism" != "dm-flakey" ]]; then
  if [[ "$dm_fault_mode_explicit" == "yes" ]]; then
    echo "error: --dm-fault-mode requires --reset-mechanism dm-flakey (or auto resolving to it)" >&2
    exit 2
  fi
  dm_fault_mode="not-applicable"
fi

mkdir -p "$output_dir" || exit 2
output_dir="$(cd "$output_dir" && pwd)"
if [[ -z "$work_root" ]]; then
  work_root="$output_dir/work"
  mkdir -p "$work_root" || exit 2
  work_root="$(cd "$work_root" && pwd)"
else
  # --work-root is a parent, never a deletion target. Own a run-specific
  # child so cleanup cannot remove pre-existing operator data.
  mkdir -p "$work_root" || exit 2
  work_parent="$(cd "$work_root" && pwd)"
  if [[ "$work_parent" == "$output_dir" || "$work_parent" == "$output_dir"/* ]]; then
    echo "error: explicit --work-root must be outside the artifact directory" >&2
    exit 2
  fi
  work_root="$work_parent/glifistore-e3-work-$$"
  if [[ -e "$work_root" ]]; then
    echo "error: generated work directory already exists: $work_root" >&2
    exit 2
  fi
  mkdir "$work_root" || exit 2
fi

provenance="$output_dir/provenance.txt"
commands="$output_dir/commands.txt"
results="$output_dir/results.tsv"
summary="$output_dir/summary.md"
cases_log="$output_dir/cases.log"
: >"$provenance"
: >"$commands"
: >"$cases_log"
printf 'iteration\tscenario\tboundary\tcheckpoint_action\tworker_stop_confirmed\treset_mechanism\tdm_fault_mode\treset_confirmed\tfsck_status\trecovery\toutcome\n' >"$results"

record_command() {
  printf '%s\n' "$*" >>"$commands"
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1"
  elif command -v sha256 >/dev/null 2>&1; then
    sha256 "$1"
  else
    return 1
  fi
}

append_command_output() {
  local label="$1"
  shift
  {
    printf '\n[%s]\n' "$label"
    "$@" 2>&1 || printf '<command failed: exit %s>\n' "$?"
  } >>"$provenance"
}

utc_now="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
source_commit="$(git -C "$root" rev-parse HEAD 2>/dev/null || printf 'not-a-git-worktree')"
source_status="$(git -C "$root" status --porcelain=v1 --untracked-files=all 2>/dev/null || true)"
source_dirty="no"
[[ -z "$source_status" ]] || source_dirty="yes"

image_path="$work_root/row.img"
mount_point="$work_root/mnt"
host_scratch="$work_root/host-scratch"
mkdir -p "$mount_point" "$host_scratch"

loop_device=""
mapper_name=""
mapper_path=""
attach_device=""
disk_id=""
mounted="no"
reset_confirmed_global="no"

terminate_worker_hard() {
  local pid="${worker_pid:-}"
  if [[ -n "$pid" ]]; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  worker_pid=""
}

cleanup_case_or_abort() {
  local candidate="$1"
  local log_file="$2"
  if [[ "$mounted" != "yes" ]]; then
    printf 'harness_cleanup=skipped-unmounted\n' >>"$log_file"
    return 0
  fi
  if [[ "$candidate" != "$mount_point"/store-?* ]]; then
    printf 'harness_cleanup=refused-unsafe-path\n' >>"$log_file"
    echo "error: refusing unsafe E3 case cleanup path: $candidate" >&2
    exit 1
  fi
  if ! rm -rf -- "$candidate"; then
    printf 'harness_cleanup=failed\n' >>"$log_file"
    echo "error: failed to reclaim E3 case data: $candidate" >&2
    exit 1
  fi
  printf 'harness_cleanup=complete\n' >>"$log_file"
}

cleanup() {
  local rc=$?
  terminate_worker_hard
  if [[ "$platform" == "linux-ext4" ]]; then
    if [[ "$mounted" == "yes" ]]; then
      sudo umount -l "$mount_point" >/dev/null 2>&1 || true
      mounted="no"
    fi
    if [[ -n "$mapper_name" ]] && [[ -e "/dev/mapper/$mapper_name" ]]; then
      sudo dmsetup remove --force "$mapper_name" >/dev/null 2>&1 || true
    fi
    if [[ -n "$loop_device" ]] && losetup "$loop_device" >/dev/null 2>&1; then
      sudo losetup -d "$loop_device" >/dev/null 2>&1 || true
    fi
  elif [[ "$platform" == "macos-apfs" ]]; then
    if [[ "$mounted" == "yes" ]] || [[ -n "$disk_id" ]]; then
      hdiutil detach -force "${disk_id:-$mount_point}" >/dev/null 2>&1 || true
      mounted="no"
    fi
  fi
  if [[ "$keep_work" != "yes" ]]; then
    rm -rf "$work_root" >/dev/null 2>&1 || true
  fi
  exit "$rc"
}
trap cleanup EXIT

need_sudo_linux() {
  if [[ "$EUID" -eq 0 ]]; then
    return 0
  fi
  if ! command -v sudo >/dev/null 2>&1; then
    echo "error: sudo is required to provision linux-ext4 loop devices" >&2
    exit 2
  fi
}

provision_linux_ext4() {
  need_sudo_linux
  record_command "dd if=/dev/zero of=<image> bs=1M count=0 seek=<size>"
  dd if=/dev/zero of="$image_path" bs=1M count=0 seek="$image_size_mib" status=none || return 1
  record_command "losetup --find --show --sector-size 512 <image>"
  loop_device="$(sudo losetup --find --show --sector-size 512 "$image_path")" || return 1
  [[ -n "$loop_device" ]] || { echo "error: losetup failed" >&2; exit 1; }
  attach_device="$loop_device"
  if [[ "$reset_mechanism" == "dm-flakey" ]]; then
    command -v dmsetup >/dev/null 2>&1 || { echo "error: dmsetup not found" >&2; exit 2; }
    local sectors
    sectors="$(sudo blockdev --getsz "$loop_device")" || return 1
    mapper_name="glifi-e3-$$"
    mapper_path="/dev/mapper/$mapper_name"
    # Keep the setup path fully available. The selected flakey down-interval
    # mode is installed only after the worker has reached the checkpoint.
    record_command "dmsetup create <mapper> --table '0 <sectors> linear <loop> 0'"
    sudo dmsetup create "$mapper_name" --table "0 $sectors linear $loop_device 0" || return 1
    attach_device="$mapper_path"
  fi
  record_command "mkfs.ext4 -F -L glifie3 <device>"
  sudo mkfs.ext4 -F -L glifie3 "$attach_device" >/dev/null || return 1
  record_command "mount -t ext4 -o defaults <device> <mount>"
  sudo mount -t ext4 -o defaults "$attach_device" "$mount_point" || return 1
  mounted="yes"
  sudo chmod 777 "$mount_point" || return 1
}

provision_macos_apfs() {
  record_command "hdiutil create -size <N>m -fs APFS -volname GlifiE3 -type SPARSEBUNDLE <image>"
  # .sparsebundle keeps the disposable row self-contained under work_root.
  local bundle="$image_path.sparsebundle"
  [[ ! -e "$bundle" ]] || return 1
  hdiutil create -size "${image_size_mib}m" -fs APFS -volname GlifiE3 -type SPARSEBUNDLE "$image_path" >/dev/null || return 1
  image_path="$bundle"
  record_command "hdiutil attach -mountpoint <mount> <image>"
  local attach_out
  attach_out="$(hdiutil attach -mountpoint "$mount_point" "$image_path")" || return 1
  # Prefer the node that is actually mounted at mount_point (APFS container + volume layout).
  disk_id="$(printf '%s\n' "$attach_out" | awk -v mp="$mount_point" 'index($0, mp) {print $1; exit}')"
  if [[ -z "$disk_id" ]]; then
    disk_id="$(printf '%s\n' "$attach_out" | awk '/\/dev\// {print $1; exit}')"
  fi
  [[ -n "$disk_id" ]] || { echo "error: failed to parse hdiutil attach device" >&2; exit 1; }
  mounted="yes"
  chmod 777 "$mount_point" 2>/dev/null || true
}

remount_linux() {
  if [[ "$mounted" == "yes" ]]; then
    sudo umount -l "$mount_point" >/dev/null 2>&1 || return 1
    mounted="no"
  fi
  if [[ -n "$mapper_name" ]] && [[ -e "/dev/mapper/$mapper_name" ]]; then
    sudo dmsetup remove --force "$mapper_name" >/dev/null 2>&1 || true
  fi
  if [[ -n "$loop_device" ]] && ! losetup "$loop_device" >/dev/null 2>&1; then
    loop_device="$(sudo losetup --find --show --sector-size 512 "$image_path")" || return 1
  fi
  attach_device="$loop_device"
  if [[ "$reset_mechanism" == "dm-flakey" ]]; then
    local sectors
    sectors="$(sudo blockdev --getsz "$loop_device")" || return 1
    mapper_name="glifi-e3-$$"
    mapper_path="/dev/mapper/$mapper_name"
    sudo dmsetup create "$mapper_name" --table "0 $sectors linear $loop_device 0" || return 1
    attach_device="$mapper_path"
  fi
  sudo mount -t ext4 -o defaults "$attach_device" "$mount_point" || return 1
  mounted="yes"
  sudo chmod 777 "$mount_point" || return 1
}

remount_macos() {
  if [[ "$mounted" == "yes" ]] || [[ -n "$disk_id" ]]; then
    hdiutil detach -force "${disk_id:-$mount_point}" >/dev/null 2>&1 || true
    mounted="no"
    disk_id=""
  fi
  local attach_out
  attach_out="$(hdiutil attach -mountpoint "$mount_point" "$image_path")" || return 1
  disk_id="$(printf '%s\n' "$attach_out" | awk -v mp="$mount_point" 'index($0, mp) {print $1; exit}')"
  if [[ -z "$disk_id" ]]; then
    disk_id="$(printf '%s\n' "$attach_out" | awk '/\/dev\// {print $1; exit}')"
  fi
  [[ -n "$disk_id" ]] || return 1
  mounted="yes"
  chmod 777 "$mount_point" 2>/dev/null || true
}

unmount_macos_for_fsck() {
  [[ -n "$disk_id" ]] || return 1
  record_command "diskutil unmount <apfs-volume>"
  diskutil unmount "$disk_id" >/dev/null || return 1
  mounted="no"
}

mount_macos_after_fsck() {
  [[ -n "$disk_id" ]] || return 1
  record_command "diskutil mount -mountPoint <mount> <apfs-volume>"
  diskutil mount -mountPoint "$mount_point" "$disk_id" >/dev/null || return 1
  mounted="yes"
  chmod 777 "$mount_point" 2>/dev/null || true
}

run_fsck() {
  local log_file="$1"
  local status="skipped"
  local -a privilege_prefix=()
  if [[ "$EUID" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1 || ! sudo -n true >/dev/null 2>&1; then
      printf 'non-interactive privilege unavailable for offline filesystem check\n' >"$log_file"
      printf 'privilege-unavailable\n'
      return 0
    fi
    privilege_prefix=(sudo -n)
  fi
  if [[ "$platform" == "linux-ext4" ]]; then
    # -n: check only; do not repair away torn-write evidence.
    if "${privilege_prefix[@]}" fsck.ext4 -n -f "$loop_device" >"$log_file" 2>&1; then
      status="clean-or-ok"
    else
      status="reported-issues"
    fi
  elif [[ "$platform" == "macos-apfs" ]]; then
    if command -v fsck_apfs >/dev/null 2>&1 && [[ -n "$disk_id" ]]; then
      if "${privilege_prefix[@]}" fsck_apfs -n "$disk_id" >"$log_file" 2>&1; then
        status="clean-or-ok"
      else
        status="reported-issues"
      fi
    else
      status="tool-unavailable"
      printf 'fsck_apfs unavailable or disk id missing\n' >"$log_file"
    fi
  fi
  printf '%s\n' "$status"
}

arm_reset_linux_abrupt() {
  # Lazy unmount + detach without sync: discards dirty page cache for the row.
  sudo umount -l "$mount_point"
  mounted="no"
  if [[ -n "$mapper_name" ]] && [[ -e "/dev/mapper/$mapper_name" ]]; then
    sudo dmsetup remove --force "$mapper_name"
    mapper_name=""
  fi
  sudo losetup -d "$loop_device"
  # Confirm the loop node is gone / not backed by our image.
  if losetup "$loop_device" >/dev/null 2>&1; then
    return 1
  else
    return 0
  fi
}

arm_reset_linux_flakey() {
  local sectors removed_name fault_table attempt remove_requested
  sectors="$(sudo blockdev --getsz "$loop_device")"
  removed_name="$mapper_name"
  case "$dm_fault_mode" in
    drop-writes)
      fault_table="0 $sectors flakey $loop_device 0 0 60 1 drop_writes"
      ;;
    error-writes)
      fault_table="0 $sectors flakey $loop_device 0 0 60 1 error_writes"
      ;;
    all-io-error)
      # No optional feature: every I/O in the down interval returns an error.
      fault_table="0 $sectors flakey $loop_device 0 0 60"
      ;;
    *)
      echo "error: unsupported dm-flakey fault mode: $dm_fault_mode" >&2
      return 1
      ;;
  esac
  # Arm a bounded down interval while the worker is confirmed stopped. A lazy
  # unmount avoids a graceful flush; only then can the stopped worker release
  # its open files so mapper removal can be confirmed rather than inferred.
  record_command "dmsetup suspend --noflush <mapper>"
  record_command "dmsetup reload <mapper> --table '<redacted> flakey <loop> 0 0 60 [$dm_fault_mode]'"
  sudo dmsetup suspend --noflush "$mapper_name" || return 1
  sudo dmsetup reload "$mapper_name" --table "$fault_table" || return 1
  sudo dmsetup resume "$mapper_name" || return 1
  printf 'dm_fault_mode_armed=yes\n'
  printf 'dm_fault_mode=%s\n' "$dm_fault_mode"
  printf 'dm_fault_window_ms=%s\n' "$dm_fault_window_ms"
  sleep 0.25
  record_command "umount -l <mount>"
  sudo umount -l "$mount_point" >/dev/null 2>&1 || return 1
  mounted="no"
  record_command "kill -KILL <stopped-worker>"
  terminate_worker_hard
  record_command "dmsetup remove --force <mapper>"
  remove_requested="no"
  for ((attempt = 0; attempt < 100; ++attempt)); do
    if sudo dmsetup remove --force "$mapper_name"; then
      remove_requested="yes"
      break
    fi
    sleep 0.05
  done
  [[ "$remove_requested" == "yes" ]] || return 1
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -e "/dev/mapper/$removed_name" ]] || break
    sleep 0.05
  done
  if [[ -e "/dev/mapper/$removed_name" ]]; then
    return 1
  else
    mapper_name=""
    return 0
  fi
}

arm_reset_macos() {
  local attempt
  # hdiutil can transiently report the APFS image busy even with -force on
  # hosted macOS. The worker remains SIGSTOP'd throughout: confirm an actual
  # below-process detach, but do not turn a failed detach into evidence.
  for attempt in 1 2 3; do
    if hdiutil detach -force "$disk_id" >/dev/null 2>&1; then
      mounted="no"
      disk_id=""
      return 0
    fi
    record_command "hdiutil detach -force <disk> retry $attempt/3"
    sleep 0.2
  done
  return 1
}

perform_reset() {
  case "$platform-$reset_mechanism" in
    linux-ext4-abrupt-detach) arm_reset_linux_abrupt ;;
    linux-ext4-dm-flakey) arm_reset_linux_flakey ;;
    macos-apfs-abrupt-detach) arm_reset_macos ;;
    *)
      echo "error: unsupported reset combination $platform/$reset_mechanism" >&2
      return 1
      ;;
  esac
}

smoke_cases() {
  printf '%s\n' \
    "put write_record" \
    "put sync_record" \
    "put write_commit_slot" \
    "put sync_commit_slot"
}

campaign_cases() {
  smoke_cases
  printf '%s\n' \
    "bootstrap sync_commit_slot" \
    "rotate sync_commit_slot#2" \
    "compact write_compaction_intent" \
    "compact rename_segment" \
    "compact sync_directory#3"
  # Wave 3 rehearsal scaffolding only — compact checkpoints expand campaign coverage
  # for pre-intent / promotion seams. Harness PASS is not E3 certification.
}

case_list="$( [[ "$profile" == "campaign" ]] && campaign_cases || smoke_cases )"

{
  printf 'schema=glifistore-durability-e3-harness-v2\n'
  printf 'generated_utc=%s\n' "$utc_now"
  printf 'collector=scripts/run-e3-block-reset.sh\n'
  printf 'platform_row=%s\n' "$platform"
  printf 'profile=%s\n' "$profile"
  printf 'reset_mechanism=%s\n' "$reset_mechanism"
  printf 'dm_fault_mode=%s\n' "$dm_fault_mode"
  printf 'dm_fault_window_ms=%s\n' "$dm_fault_window_ms"
  printf 'checkpoint_action=pause\n'
  printf 'image_size_mib=%s\n' "$image_size_mib"
  printf 'requested_repetitions=%s\n' "$repeat"
  printf 'power_loss_exercised=simulated-block-reset\n'
  printf 'physical_power_cut=no\n'
  printf 'e3_certified=no\n'
  printf 'maximum_possible_label=E3-harness\n'
  printf 'source_commit=%s\n' "$source_commit"
  printf 'source_dirty=%s\n' "$source_dirty"
  printf 'repository=%s\n' "$root"
  printf 'build_directory=%s\n' "$build_dir"
  printf 'crash_binary=%s\n' "$crash_bin"
  printf 'os_family=%s\n' "$os_name"
  printf '\n[source_status]\n%s\n' "${source_status:-<clean>}"
} >>"$provenance"

record_command "uname -a"
append_command_output "uname" uname -a
if [[ -x "$crash_bin" ]]; then
  digest_line="$(sha256_file "$crash_bin" 2>/dev/null || true)"
  {
    printf '\n[test_binaries]\n'
    printf 'glifistore_crash_persistence=%s\n' "${digest_line%% *}"
  } >>"$provenance"
fi

printf 'Provisioning %s row (%s MiB, reset=%s)...\n' "$platform" "$image_size_mib" "$reset_mechanism"
if [[ "$platform" == "linux-ext4" ]]; then
  if ! provision_linux_ext4; then
    echo "error: failed to provision linux-ext4 rehearsal row" >&2
    exit 1
  fi
  {
    printf '\n[filesystem]\n'
    findmnt -T "$mount_point" -o SOURCE,TARGET,FSTYPE,OPTIONS 2>/dev/null || df -PT "$mount_point"
    printf 'backing_image=%s\n' "$image_path"
    printf 'loop_device=%s\n' "$loop_device"
    printf 'mapper=%s\n' "${mapper_name:-none}"
    printf 'mount_options_policy=defaults (barrier-capable ext4 on loop/mapper)\n'
    printf 'guest_host_boundary=loopback-image-on-host-filesystem\n'
  } >>"$provenance"
else
  if ! provision_macos_apfs; then
    echo "error: failed to provision macos-apfs rehearsal row" >&2
    exit 1
  fi
  {
    printf '\n[filesystem]\n'
    df -P "$mount_point"
    mount | awk -v mp="$mount_point" 'index($0, " on " mp " ") > 0'
    printf 'backing_image=%s\n' "$image_path"
    printf 'disk_id=%s\n' "$disk_id"
    printf 'guest_host_boundary=hdiutil-sparsebundle-apfs\n'
  } >>"$provenance"
fi

failed=0
inconclusive=0
passed=0
iteration=1
while [[ "$iteration" -le "$repeat" ]]; do
  while read -r scenario boundary; do
    [[ -n "$scenario" ]] || continue
    case_id="${iteration}-${scenario}-${boundary//\//_}"
    data_dir="$mount_point/store-$case_id"
    checkpoint_dir="$host_scratch/checkpoints-$case_id"
    case_log="$output_dir/case-${case_id}.log"
    fsck_log="$output_dir/fsck-${case_id}.log"
    {
      printf 'iteration=%s scenario=%s boundary=%s\n' "$iteration" "$scenario" "$boundary"
      printf 'started_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >"$case_log"

    printf '# E3 harness iteration=%s scenario=%s boundary=%s reset=%s\n' \
      "$iteration" "$scenario" "$boundary" "$reset_mechanism" | tee -a "$cases_log"

    # Ensure a live mount before seeding.
    mount_ready="yes"
    if [[ "$platform" == "linux-ext4" && "$mounted" != "yes" ]]; then
      remount_linux >>"$case_log" 2>&1 || mount_ready="no"
    elif [[ "$platform" == "macos-apfs" && "$mounted" != "yes" ]]; then
      remount_macos >>"$case_log" 2>&1 || mount_ready="no"
    fi
    if [[ "$mount_ready" != "yes" ]]; then
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "no" "$reset_mechanism" "$dm_fault_mode" "no" \
        "n/a" "mount-unavailable" "INCONCLUSIVE" >>"$results"
      inconclusive=$((inconclusive + 1))
      printf 'outcome=INCONCLUSIVE reason=mount-unavailable\n' >>"$case_log"
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi

    rm -rf "$checkpoint_dir"
    mkdir -p "$checkpoint_dir" "$mount_point"
    seed_ok=0
    if "$crash_bin" --mode seed --scenario "$scenario" --data-dir "$data_dir" >>"$case_log" 2>&1; then
      seed_ok=1
    fi
    if [[ "$seed_ok" -ne 1 ]]; then
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "no" "$reset_mechanism" "$dm_fault_mode" "no" "n/a" \
        "seed-failed" "FAIL" >>"$results"
      failed=$((failed + 1))
      printf 'outcome=FAIL reason=seed-failed\n' >>"$case_log"
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi

    "$crash_bin" --mode worker --scenario "$scenario" --boundary "$boundary" \
      --checkpoint-action pause \
      --data-dir "$data_dir" --checkpoint-dir "$checkpoint_dir" >>"$case_log" 2>&1 &
    worker_pid=$!

    deadline=$((SECONDS + 60))
    marker="$checkpoint_dir/$boundary"
    while [[ ! -e "$marker" && "$SECONDS" -lt "$deadline" ]]; do
      if ! kill -0 "$worker_pid" 2>/dev/null; then
        break
      fi
      sleep 0.05
    done

    if [[ ! -e "$marker" ]]; then
      terminate_worker_hard
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "no" "$reset_mechanism" "$dm_fault_mode" "no" "n/a" \
        "checkpoint-timeout" "INCONCLUSIVE" >>"$results"
      inconclusive=$((inconclusive + 1))
      printf 'outcome=INCONCLUSIVE reason=checkpoint-timeout\n' >>"$case_log"
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi

    # Confirm the worker is stopped before detaching/faulting below the process.
    # The child self-stops in-hook; the second STOP closes the marker-observation race.
    worker_stop_confirmed="no"
    kill -STOP "$worker_pid" 2>/dev/null || true
    stop_deadline=$((SECONDS + 5))
    while [[ "$SECONDS" -lt "$stop_deadline" ]]; do
      worker_state="$(ps -o state= -p "$worker_pid" 2>/dev/null | tr -d '[:space:]')"
      if [[ "$worker_state" == T* ]]; then
        worker_stop_confirmed="yes"
        break
      fi
      if ! kill -0 "$worker_pid" 2>/dev/null; then
        break
      fi
      sleep 0.05
    done
    if [[ "$worker_stop_confirmed" != "yes" ]]; then
      terminate_worker_hard
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "no" "$reset_mechanism" "$dm_fault_mode" "no" "n/a" \
        "worker-stop-unconfirmed" "INCONCLUSIVE" >>"$results"
      inconclusive=$((inconclusive + 1))
      printf 'outcome=INCONCLUSIVE reason=worker-stop-unconfirmed\n' >>"$case_log"
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi
    printf 'worker_stop_confirmed=yes\n' >>"$case_log"

    # Worker is confirmed SIGSTOP'd at the filesystem boundary. Detach/drop below process.
    reset_confirmed="no"
    if perform_reset >>"$case_log" 2>&1; then
      reset_confirmed="yes"
      reset_confirmed_global="yes"
    fi
    terminate_worker_hard

    if [[ "$reset_confirmed" != "yes" ]]; then
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "$worker_stop_confirmed" "$reset_mechanism" \
        "$dm_fault_mode" "no" "n/a" "reset-unconfirmed" "INCONCLUSIVE" >>"$results"
      inconclusive=$((inconclusive + 1))
      printf 'outcome=INCONCLUSIVE reason=reset-unconfirmed\n' >>"$case_log"
      # Best-effort remount so later cases can proceed.
      if [[ "$platform" == "linux-ext4" ]]; then remount_linux || true; else remount_macos || true; fi
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi

    reopen_ready="yes"
    if [[ "$platform" == "linux-ext4" ]]; then
      fsck_status="$(run_fsck "$fsck_log")"
      remount_linux >>"$case_log" 2>&1 || reopen_ready="no"
    else
      # Reattach, unmount the APFS volume without detaching it, inspect the
      # offline volume, then mount that same volume for the recovery oracle.
      if ! remount_macos >>"$case_log" 2>&1 || ! unmount_macos_for_fsck >>"$case_log" 2>&1; then
        fsck_status="unavailable"
        reopen_ready="no"
      else
        fsck_status="$(run_fsck "$fsck_log")"
        mount_macos_after_fsck >>"$case_log" 2>&1 || reopen_ready="no"
      fi
    fi
    if [[ "$reopen_ready" != "yes" ]]; then
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$iteration" "$scenario" "$boundary" "pause" "$worker_stop_confirmed" "$reset_mechanism" \
        "$dm_fault_mode" "$reset_confirmed" "$fsck_status" "remount-failed" "INCONCLUSIVE" >>"$results"
      inconclusive=$((inconclusive + 1))
      printf 'fsck_status=%s\noutcome=INCONCLUSIVE reason=remount-failed\n' "$fsck_status" >>"$case_log"
      cleanup_case_or_abort "$data_dir" "$case_log"
      continue
    fi

    recovery="failed"
    if "$crash_bin" --mode verify --scenario "$scenario" --boundary "$boundary" \
      --data-dir "$data_dir" --checkpoint-dir "$checkpoint_dir" >>"$case_log" 2>&1; then
      recovery="passed"
    fi

    if [[ "$recovery" == "passed" && "$fsck_status" != "skipped" &&
          "$fsck_status" != "tool-unavailable" && "$fsck_status" != "privilege-unavailable" &&
          "$fsck_status" != "unavailable" ]]; then
      outcome="PASS"
      passed=$((passed + 1))
    elif [[ "$recovery" == "passed" ]]; then
      outcome="INCONCLUSIVE"
      inconclusive=$((inconclusive + 1))
    else
      outcome="FAIL"
      failed=$((failed + 1))
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$iteration" "$scenario" "$boundary" "pause" "$worker_stop_confirmed" "$reset_mechanism" \
      "$dm_fault_mode" "$reset_confirmed" "$fsck_status" "$recovery" "$outcome" >>"$results"
    {
      printf 'reset_confirmed=%s\n' "$reset_confirmed"
      printf 'fsck_status=%s\n' "$fsck_status"
      printf 'recovery=%s\n' "$recovery"
      printf 'outcome=%s\n' "$outcome"
      printf 'finished_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >>"$case_log"
    # Case stores contain preallocated 64 MiB Segments. Reclaim them only after
    # the recovery outcome is recorded so repetitions stay bounded by one case
    # instead of exhausting the disposable filesystem row cumulatively.
    cleanup_case_or_abort "$data_dir" "$case_log"
  done <<<"$case_list"
  iteration=$((iteration + 1))
done

harness_result="passed"
if [[ "$failed" -gt 0 ]]; then
  harness_result="failed"
elif [[ "$inconclusive" -gt 0 ]]; then
  harness_result="inconclusive"
fi

attained_label="E3-harness"
release_eligible="no"
release_reason="block-reset simulation / harness rehearsal; e3_certified remains no until a reviewed pinned campaign meets the matrix promotion rules"
if [[ "$source_dirty" == "yes" ]]; then
  release_reason="$release_reason; source worktree is dirty"
fi
if [[ "$harness_result" == "failed" ]]; then
  attained_label="FAILED"
elif [[ "$harness_result" == "inconclusive" ]]; then
  attained_label="INCONCLUSIVE"
fi

{
  printf '\n[campaign_totals]\n'
  printf 'passed=%s\n' "$passed"
  printf 'failed=%s\n' "$failed"
  printf 'inconclusive=%s\n' "$inconclusive"
  printf 'reset_confirmed_any=%s\n' "$reset_confirmed_global"
  printf 'harness_result=%s\n' "$harness_result"
  printf 'attained_label=%s\n' "$attained_label"
  printf 'e3_certified=no\n'
  printf 'e4_certified=no\n'
} >>"$provenance"

{
  printf '# GlifiStore E3 block-reset harness artifact\n\n'
  printf -- '- Generated (UTC): `%s`\n' "$utc_now"
  printf -- '- Source commit: `%s`\n' "$source_commit"
  printf -- '- Source worktree dirty: `%s`\n' "$source_dirty"
  printf -- '- Platform row: `%s`\n' "$platform"
  printf -- '- Profile: `%s`\n' "$profile"
  printf -- '- Reset mechanism: `%s`\n' "$reset_mechanism"
  printf -- '- dm-flakey fault mode: `%s`\n' "$dm_fault_mode"
  printf -- '- Result: `%s`\n' "$harness_result"
  printf -- '- Passed / failed / inconclusive: `%s` / `%s` / `%s`\n' "$passed" "$failed" "$inconclusive"
  printf -- '- Attained label: `%s`\n' "$attained_label"
  printf -- '- E3 certified: `no`\n'
  printf -- '- E4 certified: `no`\n'
  printf -- '- Release-certification eligible: `%s`\n\n' "$release_eligible"
  printf '%s\n\n' "$release_reason"
  printf 'PASS/FAIL criteria and promotion rules:\n'
  printf '`docs/architecture/platform-durability-evidence.md`.\n'
} >"$summary"

manifest="$output_dir/manifest.sha256"
: >"$manifest"
checksum_available="yes"
while IFS= read -r artifact; do
  [[ "$artifact" == "$manifest" ]] && continue
  # Skip the mutable work tree if retained.
  [[ "$artifact" == "$work_root"/* ]] && continue
  if ! digest_line="$(sha256_file "$artifact" 2>/dev/null)"; then
    checksum_available="no"
    break
  fi
  digest="${digest_line%% *}"
  printf '%s  %s\n' "$digest" "${artifact#"$output_dir/"}" >>"$manifest"
done < <(find "$output_dir" -path "$work_root" -prune -o -type f -print | LC_ALL=C sort)
if [[ "$checksum_available" == "no" ]]; then
  printf 'unavailable  no SHA-256 utility found\n' >"$manifest"
fi

printf 'E3 harness artifact written to %s\n' "$output_dir"
printf 'Result: %s; label: %s; e3_certified: no\n' "$harness_result" "$attained_label"
if ! "$root/scripts/assert-e3-rehearsal-honesty.sh" --dir "$output_dir" --kind harness; then
  echo "error: harness honesty assert failed" >&2
  exit 1
fi
if [[ "$harness_result" == "passed" ]]; then
  exit 0
fi
exit 1
