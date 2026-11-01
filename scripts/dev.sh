#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake="${CMAKE:-$root/.tools/venv/bin/cmake}"
ctest="${CTEST:-$root/.tools/venv/bin/ctest}"
export PATH="$root/.tools/venv/bin:$PATH"

require_tools() {
    if [[ ! -x "$cmake" ]]; then
        echo "Local CMake is missing. Run ./scripts/bootstrap-macos.sh first." >&2
        exit 1
    fi
}

case "${1:-help}" in
    configure)
        require_tools
        "$cmake" --preset macos-debug
        ;;
    build)
        require_tools
        "$cmake" --build --preset macos-debug
        ;;
    test)
        require_tools
        "$cmake" --preset macos-debug
        "$cmake" --build --preset macos-debug
        "$ctest" --preset macos-debug
        ;;
    asan)
        require_tools
        "$cmake" --preset macos-asan
        "$cmake" --build --preset macos-asan
        "$ctest" --preset macos-asan
        ;;
    tsan)
        require_tools
        "$cmake" --preset macos-tsan
        "$cmake" --build --preset macos-tsan
        "$ctest" --preset macos-tsan
        ;;
    benchmark)
        require_tools
        "$cmake" --preset macos-release
        "$cmake" --build --preset macos-release --target glifistore_benchmarks
        "$root/build/macos-release/glifistore_benchmarks" "${@:2}"
        ;;
    benchmark-lto)
        require_tools
        "$cmake" --preset macos-release-lto
        "$cmake" --build --preset macos-release-lto --target glifistore_benchmarks
        "$root/build/macos-release-lto/glifistore_benchmarks" "${@:2}"
        ;;
    benchmark-server)
        require_tools
        "$cmake" --preset macos-release
        "$cmake" --build --preset macos-release --target glifistore_server_benchmarks
        "$root/build/macos-release/glifistore_server_benchmarks" "${@:2}"
        ;;
    benchmark-pgo)
        require_tools
        "$cmake" --preset macos-pgo-use
        "$cmake" --build --preset macos-pgo-use --target glifistore_benchmarks
        "$root/build/macos-pgo-use/glifistore_benchmarks" "${@:2}"
        ;;
    benchmark-durable)
        require_tools
        "$cmake" --preset macos-release
        "$cmake" --build --preset macos-release --target glifistore_benchmarks
        "$root/build/macos-release/glifistore_benchmarks" --filter store-durable-all "${@:2}"
        ;;
    benchmark-compaction)
        require_tools
        "$cmake" --preset macos-release
        "$cmake" --build --preset macos-release --target glifistore_compaction_benchmark
        "$root/build/macos-release/glifistore_compaction_benchmark" "${@:2}"
        ;;
    benchmark-maintenance)
        require_tools
        "$cmake" --preset macos-release
        "$cmake" --build --preset macos-release --target glifistore_maintenance_benchmark
        "$root/build/macos-release/glifistore_maintenance_benchmark" "${@:2}"
        ;;
    pgo-generate)
        require_tools
        "$cmake" --preset macos-pgo-generate
        "$cmake" --build --preset macos-pgo-generate --target glifistore_benchmarks
        ;;
    pgo-train)
        require_tools
        "$cmake" --preset macos-pgo-generate
        "$cmake" --build --preset macos-pgo-generate --target glifistore_benchmarks glifistore_pgo_durable
        PGO_PRESET=macos-pgo-generate "$root/scripts/pgo-train.sh"
        ;;
    pgo-use)
        require_tools
        "$cmake" --preset macos-pgo-use
        "$cmake" --build --preset macos-pgo-use --target glifistore_benchmarks
        ;;
    fuzz-build)
        require_tools
        "$cmake" --preset macos-fuzz
        "$cmake" --build --preset macos-fuzz
        ;;
    fuzz-run)
        require_tools
        export GLIFISTORE_FUZZ_BUILD_DIR="${GLIFISTORE_FUZZ_BUILD_DIR:-$root/build/macos-fuzz}"
        export GLIFISTORE_FUZZ_SECONDS="${GLIFISTORE_FUZZ_SECONDS:-60}"
        bash "$root/scripts/run-fuzzers.sh" "$@"
        ;;
    test-lto)
        require_tools
        "$cmake" --preset macos-release-lto
        "$cmake" --build --preset macos-release-lto
        "$ctest" --preset macos-release-lto
        ;;
    xcode-build)
        require_tools
        "$cmake" --preset xcode
        "$cmake" --build --preset xcode-debug
        "$ctest" --preset xcode-debug
        ;;
    format)
        formatter="$(command -v clang-format || true)"
        if [[ -z "$formatter" && -x "$root/.tools/venv/bin/clang-format" ]]; then
            formatter="$root/.tools/venv/bin/clang-format"
        fi
        if [[ -z "$formatter" ]]; then
            echo "clang-format not found; install LLVM with MacPorts or Homebrew." >&2
            exit 1
        fi
        find "$root/include" "$root/src" "$root/tests" "$root/benchmarks" "$root/fuzz" \
            -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 "$formatter" -i
        ;;
    verify)
        formatter="$(command -v clang-format || true)"
        if [[ -z "$formatter" && -x "$root/.tools/venv/bin/clang-format" ]]; then
            formatter="$root/.tools/venv/bin/clang-format"
        fi
        if [[ -z "$formatter" ]]; then
            echo "clang-format not found; run ./scripts/bootstrap-macos.sh first." >&2
            exit 1
        fi
        python="${PYTHON:-$root/.tools/venv/bin/python}"
        if [[ ! -x "$python" ]]; then
            python="$(command -v python3 || true)"
        fi
        if [[ -z "$python" ]]; then
            echo "python3 not found; run ./scripts/bootstrap-macos.sh first." >&2
            exit 1
        fi
        find "$root/include" "$root/src" "$root/tests" "$root/benchmarks" "$root/fuzz" \
            -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
            | xargs -0 "$formatter" --dry-run --Werror
        while IFS= read -r -d '' script; do
            bash -n "$script"
        done < <(find "$root/scripts" -type f -name '*.sh' -print0)
        "$python" "$root/engineering/tools/validate_assurance.py"
        "$python" "$root/engineering/tools/validate_documentation.py"
        "$python" "$root/engineering/tools/validate_cmake_deps.py"
        "$python" "$root/engineering/tools/validate_structure_debt.py"
        "$python" "$root/engineering/tools/validate_actions_pins.py"
        "$python" "$root/engineering/tools/validate_compat_matrix.py"
        "$python" "$root/engineering/tools/validate_claims.py"
        "$python" "$root/engineering/tools/validate_durability_claims.py"
        "$python" "$root/engineering/tools/validate_perf_budgets.py"
        "$python" "$root/engineering/tools/validate_bsd_packaging.py"
        "$python" "$root/engineering/tools/audit_memory_orders.py"
        "$python" -m unittest discover -s "$root/scripts/tests"
        ;;
    correctness)
        "$root/scripts/run-correctness.sh"
        ;;
    clean)
        "$cmake" -E rm -rf "$root/build"
        ;;
    *)
        echo "usage: $0 {configure|build|test|asan|tsan|correctness|test-lto|benchmark|benchmark-durable|benchmark-compaction|benchmark-maintenance|benchmark-server|benchmark-lto|benchmark-pgo|pgo-generate|pgo-train|pgo-use|fuzz-build|fuzz-run|xcode-build|format|verify|clean} [benchmark args]"
        echo "PGO: pgo-generate builds instrumented benchmarks; pgo-train runs volatile + durable workloads; pgo-use rebuilds optimized benchmarks."
        echo "Fuzz: fuzz-build configures macos-fuzz; fuzz-run executes targets (GLIFISTORE_FUZZ_SECONDS, GLIFISTORE_FUZZ_BUILD_DIR)."
        echo "Correctness: complete local matrix with strict builds, hardening, sanitizers, static analysis, and explicit optional-tool gaps."
        ;;
esac
