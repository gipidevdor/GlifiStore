#!/usr/bin/env bash
# Collect reproducible native-platform durability evidence without claiming
# hardware power-loss coverage. The output is intended for CI/release artifacts.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir=""
build_dir=""
probe_path="$root"
run_mode="metadata"
repeat=1
campaign_seed=""
campaign_iterations=""

usage() {
  cat <<'EOF'
Usage:
  scripts/collect-durability-evidence.sh --output DIR [options]

Options:
  --build-dir DIR       CMake build tree containing the crash tests
  --probe-path PATH     Writable directory on the filesystem under test (default: repository)
  --run process-kill    Run all embedded and daemon SIGKILL/reopen tests
  --run random-campaign Run the reproducible randomized compaction SIGKILL/reopen campaign
  --campaign-seed N     Decimal uint64 seed required by random-campaign
  --iterations N        Random campaign cases, 1..10000
  --metadata-only       Collect provenance only (default)
  --repeat N            Repeat the selected test suite, 1..1000 (default: 1)
  -h, --help            Show this help

This collector records at most E2 process-kill evidence. It never performs,
and never labels its output as, a device reset or physical power-loss test.
For disposable block-reset rehearsal (still e3_certified=no), use
scripts/run-e3-block-reset.sh. For a full operator campaign-prep tarball
(E0→E1→E2→E3, still e3_certified=no until human promotion), use
scripts/run-e3-campaign.sh — see docs/operations/e3-campaign.md.
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
    --probe-path)
      [[ $# -ge 2 ]] || { echo "error: --probe-path requires a path" >&2; exit 2; }
      probe_path="$2"
      shift 2
      ;;
    --run)
      [[ $# -ge 2 ]] || { echo "error: --run requires a suite" >&2; exit 2; }
      run_mode="$2"
      shift 2
      ;;
    --metadata-only)
      run_mode="metadata"
      shift
      ;;
    --repeat)
      [[ $# -ge 2 ]] || { echo "error: --repeat requires a count" >&2; exit 2; }
      repeat="$2"
      shift 2
      ;;
    --campaign-seed)
      [[ $# -ge 2 ]] || { echo "error: --campaign-seed requires a decimal uint64" >&2; exit 2; }
      campaign_seed="$2"
      shift 2
      ;;
    --iterations)
      [[ $# -ge 2 ]] || { echo "error: --iterations requires a count" >&2; exit 2; }
      campaign_iterations="$2"
      shift 2
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
[[ "$run_mode" == "metadata" || "$run_mode" == "process-kill" || "$run_mode" == "random-campaign" ]] ||
  { echo "error: --run accepts process-kill|random-campaign" >&2; exit 2; }
[[ "$repeat" =~ ^[1-9][0-9]*$ && "$repeat" -le 1000 ]] ||
  { echo "error: --repeat must be in 1..1000" >&2; exit 2; }
if [[ "$run_mode" == "random-campaign" ]]; then
  [[ "$campaign_seed" =~ ^[0-9]+$ ]] ||
    { echo "error: --campaign-seed must be a decimal uint64 for random-campaign" >&2; exit 2; }
  if [[ "${#campaign_seed}" -gt 20 ]] ||
     { [[ "${#campaign_seed}" -eq 20 ]] && [[ "$campaign_seed" > "18446744073709551615" ]]; }; then
    echo "error: --campaign-seed exceeds uint64" >&2
    exit 2
  fi
  [[ "$campaign_iterations" =~ ^[1-9][0-9]*$ ]] ||
    { echo "error: --iterations must be in 1..10000 for random-campaign" >&2; exit 2; }
  [[ "$campaign_iterations" -le 10000 ]] ||
    { echo "error: --iterations must be in 1..10000 for random-campaign" >&2; exit 2; }
else
  [[ -z "$campaign_seed" && -z "$campaign_iterations" ]] ||
    { echo "error: campaign options require --run random-campaign" >&2; exit 2; }
fi
[[ -e "$probe_path" ]] || { echo "error: probe path does not exist: $probe_path" >&2; exit 2; }
[[ -d "$probe_path" ]] || { echo "error: probe path must be a directory: $probe_path" >&2; exit 2; }
if [[ -e "$output_dir" ]]; then
  echo "error: output path already exists; refusing to overwrite: $output_dir" >&2
  exit 2
fi

if [[ -n "$build_dir" ]]; then
  [[ -d "$build_dir" ]] || { echo "error: build directory does not exist: $build_dir" >&2; exit 2; }
  build_dir="$(cd "$build_dir" && pwd)"
fi
probe_path="$(cd "$probe_path" 2>/dev/null && pwd || {
  probe_parent="$(dirname "$probe_path")"
  probe_name="$(basename "$probe_path")"
  printf '%s/%s\n' "$(cd "$probe_parent" && pwd)" "$probe_name"
})"

crash_regex='^glifistore_crash_(sync|periodic|group|daemon_sync|daemon_group|daemon_periodic)$'
ctest_bin=""
if [[ -n "${CTEST:-}" ]]; then
  ctest_bin="$CTEST"
elif [[ -x "$root/.tools/venv/bin/ctest" ]]; then
  ctest_bin="$root/.tools/venv/bin/ctest"
elif command -v ctest >/dev/null 2>&1; then
  ctest_bin="$(command -v ctest)"
fi
cmake_bin=""
if [[ -x "$root/.tools/venv/bin/cmake" ]]; then
  cmake_bin="$root/.tools/venv/bin/cmake"
elif command -v cmake >/dev/null 2>&1; then
  cmake_bin="$(command -v cmake)"
fi
test_inventory=""
if [[ "$run_mode" != "metadata" ]]; then
  [[ -n "$build_dir" ]] || { echo "error: --build-dir is required for executable evidence" >&2; exit 2; }
fi
if [[ "$run_mode" == "process-kill" ]]; then
  [[ -n "$ctest_bin" && -x "$ctest_bin" ]] ||
    { echo "error: executable ctest not found (set CTEST)" >&2; exit 2; }
  [[ -f "$build_dir/CTestTestfile.cmake" ]] ||
    { echo "error: not a configured CTest build tree: $build_dir" >&2; exit 2; }
  [[ -w "$probe_path" ]] ||
    { echo "error: probe path must be writable for process-kill evidence: $probe_path" >&2; exit 2; }
  test_inventory="$("$ctest_bin" --test-dir "$build_dir" -N -R "$crash_regex" 2>&1)"
  test_count="$(printf '%s\n' "$test_inventory" | awk '/Total Tests:/ {print $3}' | tail -n 1)"
  [[ "$test_count" == "6" ]] || {
    echo "error: expected all 6 process-kill tests in $build_dir (found ${test_count:-unknown})" >&2
    printf '%s\n' "$test_inventory" >&2
    exit 2
  }
elif [[ "$run_mode" == "random-campaign" ]]; then
  [[ -x "$build_dir/glifistore_crash_persistence" ]] ||
    { echo "error: missing executable $build_dir/glifistore_crash_persistence" >&2; exit 2; }
  [[ -w "$probe_path" ]] ||
    { echo "error: probe path must be writable for random-campaign evidence: $probe_path" >&2; exit 2; }
fi

mkdir -p "$output_dir" || exit 2
output_dir="$(cd "$output_dir" && pwd)"
provenance="$output_dir/provenance.txt"
commands="$output_dir/commands.txt"
summary="$output_dir/summary.md"
: >"$provenance"
: >"$commands"

utc_now="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
os_name="$(uname -s)"
source_commit="$(git -C "$root" rev-parse HEAD 2>/dev/null || printf 'not-a-git-worktree')"
source_status="$(git -C "$root" status --porcelain=v1 --untracked-files=all 2>/dev/null || true)"
source_dirty="no"
[[ -z "$source_status" ]] || source_dirty="yes"

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

{
  printf 'schema=glifistore-durability-evidence-v1\n'
  printf 'generated_utc=%s\n' "$utc_now"
  printf 'collector=scripts/collect-durability-evidence.sh\n'
  printf 'requested_suite=%s\n' "$run_mode"
  printf 'requested_repetitions=%s\n' "$repeat"
  printf 'random_campaign_seed=%s\n' "${campaign_seed:-not-applicable}"
  printf 'random_campaign_iterations=%s\n' "${campaign_iterations:-not-applicable}"
  printf 'power_loss_exercised=no\n'
  printf 'maximum_possible_level=%s\n' "$([[ "$run_mode" == "metadata" ]] && printf 'E0' || printf 'E2')"
  printf 'source_commit=%s\n' "$source_commit"
  printf 'source_dirty=%s\n' "$source_dirty"
  printf 'repository=%s\n' "$root"
  printf 'build_directory=%s\n' "${build_dir:-not-applicable}"
  printf 'probe_path=%s\n' "$probe_path"
  printf 'os_family=%s\n' "$os_name"
  printf '\n[source_status]\n%s\n' "${source_status:-<clean>}"
} >>"$provenance"

record_command "git -C <repository> rev-parse HEAD"
record_command "git -C <repository> status --porcelain=v1 --untracked-files=all"
record_command "uname -a"
append_command_output "uname" uname -a

case "$os_name" in
  Darwin)
    record_command "sw_vers"
    append_command_output "macos" sw_vers
    {
      printf '\n[hardware]\n'
      for key in hw.model hw.machine hw.logicalcpu hw.memsize; do
        value="$(sysctl -n "$key" 2>/dev/null || printf 'unavailable')"
        printf '%s=%s\n' "$key" "$value"
      done
      printf '\n[filesystem]\n'
      df -P "$probe_path" 2>&1
      device="$(df -P "$probe_path" 2>/dev/null | awk 'END {print $1}')"
      if [[ -n "$device" ]]; then
        mount_line="$(mount | awk -v device="$device" 'index($0, device " on ") == 1')"
        fs_type="$(printf '%s\n' "$mount_line" | sed -n 's/.*(\([^,]*\).*/\1/p')"
        printf 'type=%s\n' "${fs_type:-unavailable}"
        printf '%s\n' "$mount_line"
      fi
    } >>"$provenance"
    record_command "df -P <probe-path>"
    record_command "mount (filtered to probe device)"
    ;;
  Linux)
    if [[ -r /etc/os-release ]]; then
      {
        printf '\n[linux_release]\n'
        sed -n 's/^\(NAME\\|VERSION\\|ID\\|VERSION_ID\\)=/\\1=/p' /etc/os-release
      } >>"$provenance"
      record_command "read selected fields from /etc/os-release"
    fi
    {
      printf '\n[hardware]\n'
      if command -v lscpu >/dev/null 2>&1; then
        lscpu | awk -F: '/^(Architecture|CPU\\(s\\)|Model name|Vendor ID|Hypervisor vendor|Virtualization type):/ {
          key=$1; value=$2; gsub(/^[ \t]+|[ \t]+$/, "", value); print key "=" value
        }'
      else
        printf 'lscpu=unavailable\n'
      fi
      printf '\n[filesystem]\n'
      if command -v findmnt >/dev/null 2>&1; then
        findmnt -T "$probe_path" -o SOURCE,TARGET,FSTYPE,OPTIONS
      else
        df -PT "$probe_path"
        mount
      fi
    } >>"$provenance"
    record_command "findmnt -T <probe-path> -o SOURCE,TARGET,FSTYPE,OPTIONS"
    ;;
  FreeBSD|OpenBSD)
    {
      printf '\n[hardware]\n'
      for key in hw.model hw.machine hw.ncpu hw.physmem; do
        value="$(sysctl -n "$key" 2>/dev/null || printf 'unavailable')"
        printf '%s=%s\n' "$key" "$value"
      done
      printf '\n[filesystem]\n'
      df -T "$probe_path" 2>&1
      device="$(df -P "$probe_path" 2>/dev/null | awk 'END {print $1}')"
      if [[ -n "$device" ]]; then
        mount | awk -v device="$device" 'index($0, device " on ") == 1'
      fi
    } >>"$provenance"
    record_command "df -T <probe-path>"
    record_command "mount (filtered to probe device)"
    ;;
  *)
    {
      printf '\n[filesystem]\n'
      df -P "$probe_path" 2>&1
      printf 'filesystem_detection=unsupported-os\n'
    } >>"$provenance"
    record_command "df -P <probe-path>"
    ;;
esac

if [[ -n "$cmake_bin" ]]; then
  record_command "<cmake> --version"
  append_command_output "cmake" "$cmake_bin" --version
fi
for tool in c++ cc; do
  if command -v "$tool" >/dev/null 2>&1; then
    record_command "$tool --version"
    append_command_output "$tool" "$tool" --version
  fi
done
if [[ -n "$ctest_bin" ]]; then
  record_command "<ctest> --version"
  append_command_output "ctest" "$ctest_bin" --version
fi

if [[ "$run_mode" != "metadata" ]]; then
  if [[ "$run_mode" == "process-kill" ]]; then
    printf '%s\n' "$test_inventory" >"$output_dir/test-inventory.txt"
    record_command "<ctest> --test-dir <build-directory> -N -R '$crash_regex'"
  fi
  {
    printf '\n[cmake_cache]\n'
    if [[ -r "$build_dir/CMakeCache.txt" ]]; then
      sed -n \
        -e '/^CMAKE_BUILD_TYPE:/p' \
        -e '/^CMAKE_CXX_COMPILER:/p' \
        -e '/^CMAKE_CXX_COMPILER_ID:/p' \
        -e '/^CMAKE_CXX_COMPILER_VERSION:/p' \
        -e '/^CMAKE_GENERATOR:/p' \
        -e '/^CMAKE_HOME_DIRECTORY:/p' \
        "$build_dir/CMakeCache.txt"
    else
      printf 'unavailable\n'
    fi
    printf '\n[test_binaries]\n'
    for binary_name in glifistore_crash_persistence glifistore_crash_daemon glifistored; do
      binary_path="$build_dir/$binary_name"
      if [[ -x "$binary_path" ]]; then
        digest_line="$(sha256_file "$binary_path" 2>/dev/null || true)"
        digest="${digest_line%% *}"
        printf '%s=%s\n' "$binary_name" "${digest:-sha256-unavailable}"
      else
        printf '%s=not-found\n' "$binary_name"
      fi
    done
  } >>"$provenance"
fi

test_result="not-run"
attained_level="E0"
test_exit=0
if [[ "$run_mode" == "process-kill" ]]; then
  test_result="passed"
  attained_level="E2"
  iteration=1
  while [[ "$iteration" -le "$repeat" ]]; do
    log="$output_dir/process-kill-${iteration}.log"
    record_command "TMPDIR=<probe-path> <ctest> --test-dir <build-directory> -R '$crash_regex' --output-on-failure"
    {
      printf 'iteration=%s\n' "$iteration"
      printf 'started_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
      printf 'build_directory=%s\n' "$build_dir"
      printf 'test_regex=%s\n\n' "$crash_regex"
    } >"$log"
    TMPDIR="$probe_path" "$ctest_bin" --test-dir "$build_dir" -R "$crash_regex" --output-on-failure 2>&1 |
      tee -a "$log"
    run_exit="${PIPESTATUS[0]}"
    printf '\nfinished_utc=%s\nexit_code=%s\n' \
      "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$run_exit" >>"$log"
    if [[ "$run_exit" -ne 0 ]]; then
      test_result="failed"
      attained_level="FAILED"
      test_exit=1
    fi
    iteration=$((iteration + 1))
  done
elif [[ "$run_mode" == "random-campaign" ]]; then
  test_result="passed"
  attained_level="E2"
  iteration=1
  while [[ "$iteration" -le "$repeat" ]]; do
    log="$output_dir/random-campaign-${iteration}.log"
    report="$output_dir/random-campaign-${iteration}.tsv"
    record_command "TMPDIR=<probe-path> <crash-binary> --mode random-campaign --campaign-seed <seed> --iterations <count> --report <report>"
    {
      printf 'campaign_repeat=%s\n' "$iteration"
      printf 'campaign_seed=%s\n' "$campaign_seed"
      printf 'campaign_iterations=%s\n' "$campaign_iterations"
      printf 'started_utc=%s\n\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >"$log"
    TMPDIR="$probe_path" "$build_dir/glifistore_crash_persistence" \
      --mode random-campaign \
      --campaign-seed "$campaign_seed" \
      --iterations "$campaign_iterations" \
      --report "$report" 2>&1 | tee -a "$log"
    run_exit="${PIPESTATUS[0]}"
    printf '\nfinished_utc=%s\nexit_code=%s\n' \
      "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$run_exit" >>"$log"
    if [[ "$run_exit" -ne 0 ]]; then
      test_result="failed"
      attained_level="FAILED"
      test_exit=1
    fi
    iteration=$((iteration + 1))
  done
fi

release_eligible="no"
release_reason="E0/E2 evidence cannot certify physical power-loss durability"
if [[ "$source_dirty" == "yes" ]]; then
  release_reason="$release_reason; source worktree is dirty"
fi

{
  printf '# GlifiStore durability evidence\n\n'
  printf -- '- Generated (UTC): `%s`\n' "$utc_now"
  printf -- '- Source commit: `%s`\n' "$source_commit"
  printf -- '- Source worktree dirty: `%s`\n' "$source_dirty"
  printf -- '- Requested suite: `%s`\n' "$run_mode"
  printf -- '- Repetitions: `%s`\n' "$repeat"
  if [[ "$run_mode" == "random-campaign" ]]; then
    printf -- '- Random campaign seed: `%s`\n' "$campaign_seed"
    printf -- '- Random campaign cases per repetition: `%s`\n' "$campaign_iterations"
  fi
  printf -- '- Result: `%s`\n' "$test_result"
  printf -- '- Attained evidence level: `%s`\n' "$attained_level"
  printf -- '- Physical power loss exercised: `no`\n'
  printf -- '- Release-certification eligible: `%s`\n\n' "$release_eligible"
  printf 'This artifact is not a power-loss certification: %s.\n\n' "$release_reason"
  printf 'See `provenance.txt` for the OS, hardware class, filesystem/mount, toolchain,\n'
  printf 'source state, and exact build/probe paths. See `commands.txt` and the process-kill\n'
  printf 'logs and structured campaign reports (when present) for reproduction details.\n'
} >"$summary"

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

printf 'Durability evidence written to %s\n' "$output_dir"
printf 'Result: %s; attained level: %s; power loss: no\n' "$test_result" "$attained_level"
exit "$test_exit"
