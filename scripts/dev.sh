#!/usr/bin/env bash
# GlifiStore developer entry point. Run `./scripts/dev.sh help` for the command list.
#
# Presets are chosen per platform: `macos-*` on Darwin, `unix-*` elsewhere
# (override with GLIFISTORE_PRESET_FAMILY=macos|unix). Tools resolve from the
# repository-local `.tools/venv` first, then PATH; CMAKE, CTEST and PYTHON
# override individual tools. Must stay compatible with macOS /bin/bash 3.2.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -d "$root/.tools/venv/bin" ]]; then
    export PATH="$root/.tools/venv/bin:$PATH"
fi

die() {
    echo "dev.sh: $*" >&2
    exit 1
}

host_system="$(uname -s)"
case "${GLIFISTORE_PRESET_FAMILY:-}" in
    "")
        if [[ "$host_system" == "Darwin" ]]; then family=macos; else family=unix; fi
        ;;
    macos | unix) family="$GLIFISTORE_PRESET_FAMILY" ;;
    *) die "GLIFISTORE_PRESET_FAMILY must be 'macos' or 'unix'" ;;
esac

bootstrap_hint() {
    if [[ "$host_system" == "Darwin" ]]; then
        echo "run ./scripts/bootstrap-macos.sh"
    else
        echo "install CMake >= 3.25 and Ninja, or set CMAKE/CTEST"
    fi
}

require_cmake() {
    cmake="${CMAKE:-$(command -v cmake || true)}"
    ctest="${CTEST:-$(command -v ctest || true)}"
    if [[ -z "$cmake" || -z "$ctest" ]]; then
        die "CMake/CTest not found; $(bootstrap_hint)"
    fi
}

require_python() {
    python="${PYTHON:-$(command -v python3 || true)}"
    if [[ -z "$python" ]]; then
        die "python3 not found; $(bootstrap_hint)"
    fi
}

preset_name() {
    echo "$family-$1"
}

# configure <preset> [cmake args...]
configure() {
    local preset="$1"
    shift
    "$cmake" --preset "$preset" "$@"
}

# build <preset> [cmake --build args...]; configures first when the tree is new.
build() {
    local preset="$1"
    shift
    if [[ ! -f "$root/build/$preset/CMakeCache.txt" ]]; then
        configure "$preset"
    fi
    "$cmake" --build --preset "$preset" "$@"
}

# check <preset> [ctest args...]: configure, build, then run CTest.
check() {
    local preset="$1"
    shift
    configure "$preset"
    build "$preset"
    "$ctest" --preset "$preset" "$@"
}

# bench <preset> <target> [program args...]: build one benchmark target and run it.
bench() {
    local preset="$1" target="$2"
    shift 2
    configure "$preset"
    build "$preset" --target "$target"
    "$root/build/$preset/$target" "$@"
}

verify() {
    local script step
    while IFS= read -r -d '' script; do
        bash -n "$script"
    done < <(find "$root/scripts" -type f -name '*.sh' -print0)
    require_python
    for step in \
        "format_code.py --check" \
        validate_assurance.py \
        "export_gdev_catalog.py --check" \
        validate_documentation.py \
        validate_cmake_deps.py \
        validate_structure_debt.py \
        validate_actions_pins.py \
        validate_compat_matrix.py \
        validate_claims.py \
        validate_durability_claims.py \
        validate_perf_budgets.py \
        validate_bsd_packaging.py \
        audit_memory_orders.py; do
        # Word splitting is intended: each step is "<tool> [args]".
        # shellcheck disable=SC2086
        set -- $step
        local tool="$1"
        shift
        "$python" "$root/engineering/tools/$tool" "$@" ||
            die "verify step failed: engineering/tools/$step"
    done
    "$python" -m unittest discover -s "$root/engineering/tests" -p 'test_gdev*.py'
    "$python" -m unittest discover -s "$root/scripts/tests"
}

# Lists Finder/iCloud conflict copies ("name 2", "name 2.ext") outside build output.
list_duplicate_copies() {
    find "$root" \( -path "$root/build" -o -path "$root/.tools" -o -path "$root/.ruff_cache" \) -prune \
        -o \( -name '* [0-9]' -o -name '* [0-9].*' \) -print 2>/dev/null
}

tool_line() {
    local label="$1" tool="$2"
    shift 2
    if [[ -n "$tool" ]]; then
        printf '  %-13s %s (%s)\n' "$label" "$("$tool" "$@" 2>&1 | head -1)" "$tool"
    else
        printf '  %-13s MISSING\n' "$label"
        missing=1
    fi
}

doctor() {
    local missing=0 duplicates pinned
    echo "GlifiStore development environment"
    printf '  %-13s %s (%s presets)\n' "platform" "$host_system $(uname -m)" "$family"
    tool_line cmake "${CMAKE:-$(command -v cmake || true)}" --version
    tool_line ctest "${CTEST:-$(command -v ctest || true)}" --version
    tool_line ninja "$(command -v ninja || true)" --version
    tool_line python3 "${PYTHON:-$(command -v python3 || true)}" --version
    pinned="$(<"$root/.clang-format-version")"
    tool_line clang-format "$(command -v clang-format || true)" --version
    echo "  (formatting requires clang-format $pinned exactly)"
    duplicates="$(list_duplicate_copies | wc -l | tr -d ' ')"
    if [[ "$duplicates" != "0" ]]; then
        echo
        echo "warning: $duplicates Finder/iCloud conflict copies found, for example:"
        list_duplicate_copies | head -5 | sed "s|^$root/|  |" || true
        echo "  They are ignored by Git but confuse builds, globs and reviews; compare and remove them."
    fi
    case "$root" in
        "$HOME/Documents"/* | "$HOME/Desktop"/*)
            if [[ -d "$HOME/Library/Mobile Documents/com~apple~CloudDocs/Documents" ]]; then
                echo
                echo "warning: the checkout appears to live in an iCloud-synced folder."
                echo "  iCloud creates 'name 2' conflict copies, including inside .git/."
                echo "  Prefer a non-synced location such as ~/src/GlifiStore."
            fi
            ;;
    esac
    if [[ "$missing" != "0" ]]; then
        echo
        die "required tools are missing; $(bootstrap_hint)"
    fi
}

usage() {
    cat <<EOF
usage: ./scripts/dev.sh <command> [args...]      (presets: $family-*)

Everyday
  doctor                 show detected tools, presets and checkout hazards
  configure [args]       configure the debug preset
  build [args]           build the debug preset (configures on first use);
                         e.g. build --target glifistore_tests
  test [ctest args]      configure, build and run CTest; e.g. test -R blob
  format                 apply the pinned clang-format to all native sources
  format-check           check formatting without writing
  verify                 shell syntax, formatting, assurance validators, script tests
  clean                  remove build/

Sanitizers and correctness
  asan [ctest args]      AddressSanitizer + UBSan build and tests
  tsan [ctest args]      ThreadSanitizer build and tests
  test-lto [ctest args]  release LTO build and tests
  correctness            full local matrix (strict builds, hardening, sanitizers,
                         static analysis, explicit optional-tool gaps)
  fuzz-build             configure and build the libFuzzer preset
  fuzz-run [args]        run fuzzers (GLIFISTORE_FUZZ_SECONDS, GLIFISTORE_FUZZ_BUILD_DIR)

Benchmarks (extra args go to the benchmark binary)
  benchmark              engine suite (release)
  benchmark-durable      engine suite, durable filter
  benchmark-compaction   compaction benchmark
  benchmark-maintenance  maintenance controller benchmark
  benchmark-server       daemon/wire benchmark
  benchmark-lto          engine suite, LTO build
  benchmark-pgo          engine suite, PGO-optimized build
  pgo-generate | pgo-train | pgo-use
                         build instrumented, train volatile + durable, rebuild optimized

macOS only
  xcode-build            generate Xcode project, build and test

Environment: GLIFISTORE_PRESET_FAMILY=macos|unix, CMAKE, CTEST, PYTHON.
EOF
}

command="${1:-help}"
if [[ $# -gt 0 ]]; then
    shift
fi

case "$command" in
    help | -h | --help) usage ;;
    doctor) doctor ;;
    configure)
        require_cmake
        configure "$(preset_name debug)" "$@"
        ;;
    build)
        require_cmake
        build "$(preset_name debug)" "$@"
        ;;
    test)
        require_cmake
        check "$(preset_name debug)" "$@"
        ;;
    asan | tsan)
        require_cmake
        check "$(preset_name "$command")" "$@"
        ;;
    test-lto)
        require_cmake
        check "$(preset_name release-lto)" "$@"
        ;;
    correctness) "$root/scripts/run-correctness.sh" "$@" ;;
    fuzz-build)
        require_cmake
        configure "$(preset_name fuzz)"
        build "$(preset_name fuzz)"
        ;;
    fuzz-run)
        export GLIFISTORE_FUZZ_BUILD_DIR="${GLIFISTORE_FUZZ_BUILD_DIR:-$root/build/$(preset_name fuzz)}"
        export GLIFISTORE_FUZZ_SECONDS="${GLIFISTORE_FUZZ_SECONDS:-60}"
        bash "$root/scripts/run-fuzzers.sh" "$@"
        ;;
    benchmark)
        require_cmake
        bench "$(preset_name release)" glifistore_benchmarks "$@"
        ;;
    benchmark-durable)
        require_cmake
        bench "$(preset_name release)" glifistore_benchmarks --filter store-durable-all "$@"
        ;;
    benchmark-compaction)
        require_cmake
        bench "$(preset_name release)" glifistore_compaction_benchmark "$@"
        ;;
    benchmark-maintenance)
        require_cmake
        bench "$(preset_name release)" glifistore_maintenance_benchmark "$@"
        ;;
    benchmark-server)
        require_cmake
        bench "$(preset_name release)" glifistore_server_benchmarks "$@"
        ;;
    benchmark-lto)
        require_cmake
        bench "$(preset_name release-lto)" glifistore_benchmarks "$@"
        ;;
    benchmark-pgo)
        require_cmake
        bench "$(preset_name pgo-use)" glifistore_benchmarks "$@"
        ;;
    pgo-generate)
        require_cmake
        configure "$(preset_name pgo-generate)"
        build "$(preset_name pgo-generate)" --target glifistore_benchmarks
        ;;
    pgo-train)
        require_cmake
        configure "$(preset_name pgo-generate)"
        build "$(preset_name pgo-generate)" --target glifistore_benchmarks glifistore_pgo_durable
        PGO_PRESET="$(preset_name pgo-generate)" "$root/scripts/pgo-train.sh"
        ;;
    pgo-use)
        require_cmake
        configure "$(preset_name pgo-use)"
        build "$(preset_name pgo-use)" --target glifistore_benchmarks
        ;;
    xcode-build)
        [[ "$host_system" == "Darwin" ]] || die "xcode-build requires macOS"
        require_cmake
        configure xcode
        "$cmake" --build --preset xcode-debug
        "$ctest" --preset xcode-debug "$@"
        ;;
    format | format-check)
        require_python
        mode="--write"
        if [[ "$command" == "format-check" ]]; then mode="--check"; fi
        "$python" "$root/engineering/tools/format_code.py" "$mode" "$@"
        ;;
    verify) verify ;;
    clean) rm -rf "$root/build" ;;
    *)
        echo "dev.sh: unknown command '$command'" >&2
        echo "Run ./scripts/dev.sh help for the command list." >&2
        exit 2
        ;;
esac
