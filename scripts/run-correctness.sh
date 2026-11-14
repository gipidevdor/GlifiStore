#!/usr/bin/env bash
# Reproducible, layered local correctness campaign.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake="${CMAKE:-$root/.tools/venv/bin/cmake}"
ctest="${CTEST:-$root/.tools/venv/bin/ctest}"
python="${PYTHON:-$root/.tools/venv/bin/python}"
output_dir="${GLIFISTORE_CORRECTNESS_OUTPUT_DIR:-$root/build/correctness}"
summary="$output_dir/local-correctness-summary.tsv"
mkdir -p "$output_dir"
: > "$summary"

required_failures=0
optional_unavailable=0

execute() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

record() {
    local name="$1"
    local status="$2"
    local detail="$3"
    printf '%s\t%s\t%s\n' "$name" "$status" "$detail" | tee -a "$summary"
}

run_required() {
    local name="$1"
    shift
    echo "== REQUIRED: $name =="
    if execute "$@"; then
        record "$name" PASS "exit=0"
    else
        local status=$?
        record "$name" FAIL "exit=$status"
        required_failures=$((required_failures + 1))
    fi
}

run_optional() {
    local name="$1"
    shift
    echo "== OPTIONAL: $name =="
    if execute "$@"; then
        record "$name" PASS "exit=0"
    else
        local status=$?
        record "$name" FAIL "exit=$status"
        required_failures=$((required_failures + 1))
    fi
}

unavailable() {
    local name="$1"
    local reason="$2"
    echo "== OPTIONAL_UNAVAILABLE: $name: $reason =="
    record "$name" OPTIONAL_UNAVAILABLE "$reason"
    optional_unavailable=$((optional_unavailable + 1))
}

configure_build_test() {
    local preset="$1"
    execute "$cmake" --preset "$preset" &&
        execute "$cmake" --build --preset "$preset" &&
        execute "$ctest" --preset "$preset"
}

run_asan() {
    local preset="$1"
    local pointer_pair_mode=OFF
    local pointer_pair_compiler="${CXX:-$(command -v c++ || true)}"
    local pointer_pair_probe="$output_dir/pointer-pair-sanitizer-probe"
    local pointer_pair_log="$output_dir/pointer-pair-sanitizer-probe.log"
    if [[ -n "$pointer_pair_compiler" ]] &&
        execute "$pointer_pair_compiler" -std=c++23 -fsanitize=address,pointer-compare,pointer-subtract \
            -fno-omit-frame-pointer "$root/scripts/probes/pointer_pair_sanitizer_probe.cpp" \
            -o "$pointer_pair_probe" >"$pointer_pair_log" 2>&1 &&
        execute env ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:detect_invalid_pointer_pairs=2" \
            "$pointer_pair_probe" >>"$pointer_pair_log" 2>&1; then
        pointer_pair_mode=ON
    else
        echo "pointer compare/subtract sanitizer unavailable: functional probe failed"
    fi

    execute "$cmake" --preset "$preset" \
        -DGLIFISTORE_ENABLE_POINTER_PAIR_SANITIZER="$pointer_pair_mode" &&
        execute "$cmake" --build --preset "$preset" || return

    local help_file="$output_dir/asan-options.txt"
    execute env GLIFISTORE_TEST_FILTER="__asan_option_probe__" \
        ASAN_OPTIONS="help=1:detect_leaks=0" \
        "$root/build/$preset/glifistore_tests" > /dev/null 2>"$help_file" || true
    local options="detect_leaks=${asan_detect_leaks}:halt_on_error=1:abort_on_error=1"
    for option in check_initialization_order detect_stack_use_after_return strict_string_checks \
        alloc_dealloc_mismatch; do
        if rg -q "^[[:space:]]*$option$" "$help_file"; then
            local option_probe="$output_dir/asan-$option-probe.txt"
            execute env GLIFISTORE_TEST_FILTER="__asan_option_probe__" \
                ASAN_OPTIONS="detect_leaks=0:$option=1" \
                "$root/build/$preset/glifistore_tests" > /dev/null 2>"$option_probe" || true
            if ! rg -qi "not supported|unrecognized flag|unknown flag" "$option_probe"; then
                options="$options:$option=1"
            fi
        fi
    done
    if [[ "$pointer_pair_mode" == "ON" ]]; then
        options="$options:detect_invalid_pointer_pairs=2"
    fi
    echo "ASAN_OPTIONS=$options"
    execute env ASAN_OPTIONS="$options" \
        UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
        "$ctest" --preset "$preset"
}

run_tsan() {
    local preset="$1"
    execute "$cmake" --preset "$preset" &&
        execute "$cmake" --build --preset "$preset" &&
        execute env TSAN_OPTIONS="halt_on_error=1" "$ctest" --preset "$preset"
}

run_gcc_diversity() {
    local compiler="$1"
    local build="$output_dir/gcc-debug"
    execute env CXX="$compiler" "$cmake" -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_EXTENSIONS=OFF \
        -DGLIFISTORE_WARNINGS_AS_ERRORS=ON -DGLIFISTORE_FAULT_INJECTION=ON &&
        execute "$cmake" --build "$build" &&
        execute "$ctest" --test-dir "$build" --output-on-failure
}

run_scan_build() {
    local scanner="$1"
    local build="$output_dir/scan-build"
    execute "$cmake" -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTING=OFF -DGLIFISTORE_BUILD_BENCHMARKS=OFF &&
        execute "$scanner" --status-bugs "$cmake" --build "$build" --clean-first
}

run_fuzz_smoke() {
    local preset="$1"
    local configure_log="$output_dir/fuzz-configure.log"
    if ! execute "$cmake" --preset "$preset" >"$configure_log" 2>&1; then
        if rg -q "does not provide a linkable libFuzzer runtime|Fuzz targets require Clang/libFuzzer" \
            "$configure_log"; then
            unavailable "libFuzzer smoke" "selected compiler has no linkable libFuzzer runtime"
            return 0
        fi
        cat "$configure_log"
        return 1
    fi
    cat "$configure_log"
    execute "$cmake" --build --preset "$preset" &&
        execute env GLIFISTORE_FUZZ_BUILD_DIR="$root/build/$preset" \
        GLIFISTORE_FUZZ_SECONDS="${GLIFISTORE_FUZZ_SECONDS:-30}" \
        "$root/scripts/run-fuzzers.sh"
}

run_lsan_probe() {
    local compiler="${CXX:-$(command -v c++ || true)}"
    local binary="$output_dir/lsan-probe"
    local log="$output_dir/lsan-probe.log"
    if [[ -z "$compiler" ]] || ! execute "$compiler" -std=c++23 -fsanitize=address -fno-omit-frame-pointer \
        "$root/scripts/probes/lsan_probe.cpp" -o "$binary" >"$log" 2>&1; then
        cat "$log"
        return 1
    fi
    execute env ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" "$binary" >"$log" 2>&1
    local status=$?
    cat "$log"
    if rg -qi "LeakSanitizer.*not supported|detect_leaks.*not supported" "$log"; then
        unavailable "standalone LeakSanitizer" "the selected ASan runtime explicitly reports leak detection unsupported"
        return 0
    fi
    if [[ "$status" -ne 0 ]] && rg -q "LeakSanitizer: detected memory leaks" "$log"; then
        return 0
    fi
    echo "LeakSanitizer probe did not diagnose the intentional leak" >&2
    return 1
}

if [[ ! -x "$cmake" || ! -x "$ctest" || ! -x "$python" ]]; then
    echo "correctness campaign requires the repository CMake, CTest, and Python environment" >&2
    exit 2
fi

case "$(uname -s)" in
    Darwin)
        native_prefix="macos"
        asan_detect_leaks=0
        ;;
    *)
        native_prefix="unix"
        asan_detect_leaks=1
        ;;
esac

clang_tidy="${CLANG_TIDY:-}"
if [[ -z "$clang_tidy" ]]; then
    clang_tidy="$(command -v clang-tidy || true)"
fi
if [[ -z "$clang_tidy" && -x "$root/.tools/venv/bin/clang-tidy" ]]; then
    clang_tidy="$root/.tools/venv/bin/clang-tidy"
fi

run_required "repository verification" "$root/scripts/dev.sh" verify
run_required "Debug build and complete CTest" configure_build_test "$native_prefix-debug"
run_required "strict warnings-as-errors build and CTest" configure_build_test "unix-strict"

if [[ -n "$clang_tidy" ]]; then
    run_required "high-signal production clang-tidy" "$python" \
        "$root/engineering/tools/run_clang_tidy_gate.py" \
        --build-dir "$root/build/$native_prefix-debug" --clang-tidy "$clang_tidy"
    run_required "broad production clang-tidy with reviewed triage" "$python" \
        "$root/engineering/tools/run_correctness_tidy.py" \
        --build-dir "$root/build/$native_prefix-debug" --clang-tidy "$clang_tidy" \
        --triage "$root/engineering/correctness/clang-tidy-triage.json" --fail-on ab \
        --output "$output_dir/clang-tidy-report.json"
else
    record "high-signal production clang-tidy" FAIL "clang-tidy executable not found"
    record "broad production clang-tidy with reviewed triage" FAIL "clang-tidy executable not found"
    required_failures=$((required_failures + 2))
fi

run_required "diagnostic standard-library hardening" configure_build_test "$native_prefix-correctness"
run_required "ASan+UBSan fail-fast matrix" run_asan "$native_prefix-asan"
run_required "TSan fail-fast matrix" run_tsan "$native_prefix-tsan"

gcc_compiler=""
for candidate in g++-15 g++-14 g++-13 g++-12 g++; do
    resolved="$(command -v "$candidate" || true)"
    if [[ -n "$resolved" ]] && "$resolved" --version 2>&1 | head -1 | rg -qi "gcc|g\+\+|free software foundation"; then
        gcc_compiler="$resolved"
        break
    fi
done
if [[ -n "$gcc_compiler" ]]; then
    run_optional "GCC compiler diversity" run_gcc_diversity "$gcc_compiler"
else
    unavailable "GCC compiler diversity" "no genuine GCC C++ compiler found"
fi

scan_build="$(command -v scan-build || true)"
if [[ -n "$scan_build" ]]; then
    run_optional "Clang static analyzer diversity" run_scan_build "$scan_build"
else
    unavailable "Clang static analyzer diversity" "scan-build executable not found"
fi

echo "== OPTIONAL: standalone LeakSanitizer capability probe =="
if run_lsan_probe; then
    if ! tail -1 "$summary" | rg -q '^standalone LeakSanitizer[[:space:]]+OPTIONAL_UNAVAILABLE'; then
        record "standalone LeakSanitizer" PASS "intentional leak was diagnosed"
    fi
else
    status=$?
    record "standalone LeakSanitizer" FAIL "exit=$status or intentional leak was missed"
    required_failures=$((required_failures + 1))
fi

echo "== OPTIONAL: libFuzzer smoke =="
if run_fuzz_smoke "$native_prefix-fuzz"; then
    if ! tail -1 "$summary" | rg -q '^libFuzzer smoke[[:space:]]+OPTIONAL_UNAVAILABLE'; then
        record "libFuzzer smoke" PASS "${GLIFISTORE_FUZZ_SECONDS:-30}s per target"
    fi
else
    status=$?
    record "libFuzzer smoke" FAIL "exit=$status"
    required_failures=$((required_failures + 1))
fi

echo "== correctness summary =="
column -t -s $'\t' "$summary" 2>/dev/null || cat "$summary"
echo "required_failures=$required_failures optional_unavailable=$optional_unavailable"
if [[ "$required_failures" -ne 0 ]]; then
    exit 1
fi
