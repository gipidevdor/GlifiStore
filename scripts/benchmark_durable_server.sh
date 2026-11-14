#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake="${CMAKE:-$root/.tools/venv/bin/cmake}"
preset="${BENCHMARK_PRESET:-macos-release}"
binary="$root/build/$preset/glifistore_server_benchmarks"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
output="${1:-$root/benchmark-results/durable-tcp-$stamp}"
operations="${BENCHMARK_OPS:-500}"
latency_operations="${BENCHMARK_LATENCY_OPS:-$operations}"
repeats="${BENCHMARK_REPEATS:-3}"
warmup="${BENCHMARK_WARMUP:-1}"
value_size="${BENCHMARK_VALUE_SIZE:-64}"
configurations="${BENCHMARK_CONFIGURATIONS:-1:1 2:2 4:4 1:4 2:8 4:16}"
pipelines="${BENCHMARK_PIPELINES:-1 8 32}"
modes="${BENCHMARK_STORAGE_MODES:-durable-sync durable-group durable-periodic}"

mkdir -p "$output"
"$cmake" --preset "$preset"
"$cmake" --build --preset "$preset" --target glifistore_server_benchmarks

{
    echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_sha=$(git -C "$root" rev-parse HEAD)"
    echo "git_describe=$(git -C "$root" describe --always --dirty)"
    echo "preset=$preset"
    echo "operations=$operations"
    echo "latency_operations=$latency_operations"
    echo "repeats=$repeats"
    echo "warmup=$warmup"
    echo "value_size=$value_size"
    echo "configurations=$configurations"
    echo "pipelines=$pipelines"
    echo "storage_modes=$modes"
    uname -a
    sysctl -n machdep.cpu.brand_string 2>/dev/null || true
    sysctl -n hw.memsize 2>/dev/null || true
    df -h "$output"
} > "$output/environment.txt"

for configuration in $configurations; do
    workers="${configuration%%:*}"
    clients="${configuration##*:}"
    for mode in $modes; do
        for pipeline in $pipelines; do
            stem="${mode}-w${workers}-c${clients}-p${pipeline}"
            common=(
                --workers "$workers"
                --clients "$clients"
                --pipeline "$pipeline"
                --value-size "$value_size"
                --storage-mode "$mode"
                --group-max-records 32
                --group-max-bytes 65536
                --group-max-wait-ms 10
                --periodic-sync-ms 1000
                --warmup "$warmup"
                --repeats "$repeats"
            )
            "$binary" "${common[@]}" --ops "$operations" > "$output/$stem-throughput.txt"
            "$binary" "${common[@]}" --ops "$latency_operations" --latency \
                > "$output/$stem-latency.txt"
        done
    done
done

python3 "$root/scripts/benchmark_report.py" "$output"/*.txt \
    --json "$output/results.json" \
    --markdown "$output/summary.md"

echo "$output"
