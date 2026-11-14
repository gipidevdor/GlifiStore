#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${PGO_PRESET:-macos-pgo-generate}"
build_dir="$root/build/${preset}"
profile_dir="${PGO_PROFILE_DIR:-$root/build/pgo-profiles}"
merged_profile="$profile_dir/merged.profdata"
benchmarks="${PGO_BENCHMARKS:-$build_dir/glifistore_benchmarks}"

find_llvm_tool() {
    local name="$1"
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    local candidate
    for candidate in "$name"-*; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    if [[ -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/$name" ]]; then
        echo "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/$name"
        return 0
    fi
    return 1
}

if [[ ! -x "$benchmarks" ]]; then
    echo "Instrumented benchmark binary not found at $benchmarks" >&2
    echo "Configure and build the ${preset} preset first." >&2
    exit 1
fi

mkdir -p "$profile_dir"
rm -f "$profile_dir"/*.profraw "$merged_profile"

export LLVM_PROFILE_FILE="$profile_dir/glifistore-%p-%m.profraw"

echo "# PGO training profile dir: $profile_dir"
echo "# Running GlifiStore benchmark workload"

run_case() {
    echo "# benchmark $*"
    "$benchmarks" "$@"
}

run_case --filter index-all --ops 100000 --repeats 2 --warmup 1 --key-size 16
run_case --filter index-all --ops 50000 --repeats 1 --warmup 1 --key-size 256
run_case --filter store-put --ops 200000 --repeats 2 --warmup 1 --key-size 16
run_case --filter store-get --ops 200000 --repeats 2 --warmup 1 --key-size 16
run_case --filter store-read-after-write --ops 100000 --repeats 2 --warmup 1 --key-size 16
run_case --filter store-put-get --ops 100000 --repeats 1 --warmup 1 --key-size 16 --random
run_case --filter store-parallel-all --ops 50000 --repeats 1 --warmup 1 --key-size 16 \
    --workers 4 --threads 4 --distribution worker-affine
run_case --filter store-parallel-all --ops 50000 --repeats 1 --warmup 1 --key-size 16 \
    --workers 4 --threads 4 --distribution uniform

run_case --filter store-durable-put --ops 4096 --repeats 1 --warmup 1 --key-size 16 --workers 1
run_case --filter store-durable-get --ops 4096 --repeats 1 --warmup 1 --key-size 16 --workers 1
run_case --filter store-durable-read-after-write --ops 2048 --repeats 1 --warmup 1 --key-size 16 --workers 1
run_case --filter store-durable-periodic-read-after-write --ops 20000 --repeats 1 --warmup 1 --key-size 16 --workers 1
run_case --filter store-durable-recovery-open --ops 4096 --repeats 1 --warmup 1 --key-size 16 --workers 1
run_case --filter store-durable-parallel-all --ops 2048 --repeats 1 --warmup 1 --key-size 16 \
    --workers 4 --threads 4 --distribution worker-affine

pgo_durable="${PGO_DURABLE:-$root/build/${preset}/glifistore_pgo_durable}"
if [[ -x "$pgo_durable" ]]; then
    echo "# durable PGO legacy binary: $pgo_durable (optional supplement)"
    "$pgo_durable" "${PGO_DURABLE_OPS:-1024}" || true
else
    echo "# durable PGO legacy binary not found at $pgo_durable; durable filters above cover the hot path"
fi

shopt -s nullglob
profraw_files=("$profile_dir"/*.profraw)
if [[ ${#profraw_files[@]} -eq 0 ]]; then
    echo "No .profraw files were produced in $profile_dir" >&2
    exit 1
fi

if llvm_profdata="$(find_llvm_tool llvm-profdata)"; then
    "$llvm_profdata" merge -output="$merged_profile" "${profraw_files[@]}"
    echo "# merged Clang profile: $merged_profile"
    exit 0
fi

if compgen -G "$profile_dir/*.gcda" >/dev/null; then
    echo "# GCC profile data generated under $profile_dir"
    echo "# Configure unix-pgo-use to consume it."
    exit 0
fi

echo "Training finished but no mergeable profile was found." >&2
echo "Install llvm-profdata (LLVM toolchain) to merge Clang profiles." >&2
exit 1
