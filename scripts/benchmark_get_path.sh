#!/usr/bin/env bash
# Comparative durable GET path microbenchmarks for hot-cache / prepare_get work.
# Usage: ./scripts/benchmark_get_path.sh [label] [extra glifistore_benchmarks args...]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
label="${1:-baseline}"
shift || true
out_dir="${root}/benchmark-results/get-path-${label}"
mkdir -p "${out_dir}"

cmake="${CMAKE:-cmake}"
if [[ -x "${root}/.tools/venv/bin/cmake" ]]; then
  cmake="${root}/.tools/venv/bin/cmake"
fi
"${cmake}" --build --preset macos-release --target glifistore_benchmarks

bench="${root}/build/macos-release/glifistore_benchmarks"
# Higher defaults: 500-ops medians were too noisy to judge ~µs hot GETs.
common=(--warmup 2 --repeats 5)

run() {
  local name="$1"
  shift
  echo "==> ${name}"
  "${bench}" "${common[@]}" "$@" | tee "${out_dir}/${name}.txt"
}

ops="${GET_PATH_BENCH_OPS:-5000}"
parallel_ops="${GET_PATH_BENCH_PARALLEL_OPS:-8000}"
for workers in 1 2 4 8; do
  for value in 32 256 4096 65536; do
    run "durable-get-w${workers}-v${value}" \
      --filter store-durable-get --workers "${workers}" --value-size "${value}" \
      --ops "${ops}" "$@"
  done
  run "durable-put-get-w${workers}-v256" \
    --filter store-durable-put-get --workers "${workers}" --value-size 256 \
    --ops "${ops}" "$@"
  run "durable-parallel-get-w${workers}-v256-uniform" \
    --filter store-durable-parallel-get --workers "${workers}" --threads "${workers}" \
    --value-size 256 --distribution uniform --ops "${parallel_ops}" --latency "$@"
  run "durable-parallel-get-w${workers}-v256-zipf" \
    --filter store-durable-parallel-get --workers "${workers}" --threads "${workers}" \
    --value-size 256 --distribution zipf --ops "${parallel_ops}" --latency "$@"
done

# One oversized value to exercise admission size-reject / cold path.
run "durable-get-w2-v131072" \
  --filter store-durable-get --workers 2 --value-size 131072 \
  --ops 200 "$@"

echo "Wrote results under ${out_dir}"
