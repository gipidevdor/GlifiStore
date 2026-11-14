#!/usr/bin/env bash
# Run GlifiStore libFuzzer targets with a bounded wall-clock budget.
#
# Intended for:
#   - GitHub Actions (Sanitizers workflow fuzz-run job)
#   - Local smoke after `cmake --preset unix-fuzz` / `macos-fuzz`
#
# Environment:
#   GLIFISTORE_FUZZ_BUILD_DIR   build directory (default: build/unix-fuzz)
#   GLIFISTORE_FUZZ_SECONDS     max_total_time per target (default: 60)
#   GLIFISTORE_FUZZ_TARGET      single target name, or empty for all
#   GLIFISTORE_FUZZ_JOBS        libFuzzer -jobs (default: 1)
#   GLIFISTORE_FUZZ_TIMEOUT     per-input timeout seconds (default: 10)
#   GLIFISTORE_FUZZ_CORPUS_ROOT corpus root (default: fuzz/corpus)
#   GLIFISTORE_FUZZ_ARTIFACT_DIR crash/timeout artifact dir (default: build dir)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="${GLIFISTORE_FUZZ_BUILD_DIR:-$root/build/unix-fuzz}"
seconds="${GLIFISTORE_FUZZ_SECONDS:-60}"
jobs="${GLIFISTORE_FUZZ_JOBS:-1}"
timeout_s="${GLIFISTORE_FUZZ_TIMEOUT:-10}"
corpus_root="${GLIFISTORE_FUZZ_CORPUS_ROOT:-$root/fuzz/corpus}"
artifact_dir="${GLIFISTORE_FUZZ_ARTIFACT_DIR:-$build_dir/fuzz-artifacts}"
selected="${GLIFISTORE_FUZZ_TARGET:-}"

all_targets=(record_decoder segment_scanner index_rebuild protocol_decoder store_state_machine)

if [[ -n "$selected" ]]; then
  targets=("$selected")
else
  targets=("${all_targets[@]}")
fi

if ! [[ "$seconds" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: GLIFISTORE_FUZZ_SECONDS must be a positive integer (got '$seconds')" >&2
  exit 1
fi

mkdir -p "$artifact_dir"

echo "== GlifiStore continuous fuzz =="
echo "build_dir=$build_dir"
echo "seconds_per_target=$seconds"
echo "jobs=$jobs"
echo "timeout_s=$timeout_s"
echo "corpus_root=$corpus_root"
echo "artifact_dir=$artifact_dir"
echo "targets=${targets[*]}"

failed=0
for target in "${targets[@]}"; do
  bin="$build_dir/fuzz_${target}"
  corpus="$corpus_root/$target"
  if [[ ! -x "$bin" ]]; then
    echo "error: missing executable $bin (configure/build unix-fuzz or macos-fuzz first)" >&2
    exit 1
  fi
  if [[ ! -d "$corpus" ]]; then
    echo "error: missing seed corpus directory $corpus" >&2
    exit 1
  fi

  echo "== fuzz_${target} (${seconds}s) =="
  # -rss_limit_mb=0 disables the default RSS abort which is noisy on shared CI runners.
  # Non-zero exit means crash, timeout-as-failure, OOM, or ASan/UBSan issue.
  fuzz_args=(
    -max_total_time="$seconds"
    -timeout="$timeout_s"
    -rss_limit_mb=0
    -artifact_prefix="$artifact_dir/${target}-"
  )
  if [[ "$jobs" =~ ^[1-9][0-9]*$ ]] && [[ "$jobs" -gt 1 ]]; then
    fuzz_args+=(-jobs="$jobs" -workers="$jobs")
  fi
  set +e
  "$bin" "${fuzz_args[@]}" "$corpus"
  status=$?
  set -e
  if [[ "$status" -ne 0 ]]; then
    echo "error: fuzz_${target} exited with status $status" >&2
    failed=1
  else
    echo "fuzz_${target} OK"
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "Continuous fuzz FAILED; inspect $artifact_dir" >&2
  ls -la "$artifact_dir" || true
  exit 1
fi

echo "Continuous fuzz OK"
