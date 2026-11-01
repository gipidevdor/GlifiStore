#!/usr/bin/env bash
# Diagnostic coverage report (not a merge gate). See docs/development/test-strategy.md §7.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

outdir="${GLIFISTORE_COVERAGE_OUT:-$root/build/coverage}"
builddir="$root/build/unix-coverage"
rm -rf "$outdir" "$builddir"
mkdir -p "$outdir"

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

compiler_version="$("$CXX" --version)"
clang_coverage=false
coverage_flags=--coverage
if grep -qi clang <<<"$compiler_version"; then
  clang_coverage=true
  coverage_flags='-fprofile-instr-generate -fcoverage-mapping'
  clang_major="$("$CXX" -dumpversion | cut -d. -f1)"
  compiler_dir="$(dirname "$(command -v "$CXX")")"
  find_llvm_tool() {
    local tool="$1" override="$2" found
    if [[ -n "$override" ]]; then
      command -v "$override"
      return
    fi
    found="$(command -v "$compiler_dir/$tool" || command -v "$tool-$clang_major" \
      || command -v "$tool" || true)"
    if [[ -z "$found" ]]; then
      echo "$tool is required to decode Clang coverage data" >&2
      return 1
    fi
    echo "$found"
  }
  coverage_llvm_cov="$(find_llvm_tool llvm-cov "${LLVM_COV:-}")"
  coverage_llvm_profdata="$(find_llvm_tool llvm-profdata "${LLVM_PROFDATA:-}")"
fi
export CFLAGS="${CFLAGS:-$coverage_flags -O0 -g}"
export CXXFLAGS="${CXXFLAGS:-$coverage_flags -O0 -g}"
export LDFLAGS="${LDFLAGS:-$coverage_flags}"

cmake -S "$root" -B "$builddir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
  -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
  -DGLIFISTORE_FAULT_INJECTION=ON \
  -DBUILD_TESTING=ON

cmake --build "$builddir" --target glifistore_tests
LLVM_PROFILE_FILE="$outdir/%p.profraw" \
  ctest --test-dir "$builddir" --output-on-failure --tests-regex '^glifistore_tests$'

if ! command -v lcov >/dev/null 2>&1; then
  echo "lcov is required to generate the diagnostic coverage report" >&2
  exit 1
fi

if [[ "$clang_coverage" == true ]]; then
  # Native LLVM coverage avoids the gcov adapter's synthetic line-zero functions.
  # Keep malformed reports fatal instead of ignoring lcov's format errors.
  "$coverage_llvm_profdata" merge -sparse "$outdir"/*.profraw -o "$outdir/coverage.profdata"
  "$coverage_llvm_cov" export --format=lcov \
    --instr-profile="$outdir/coverage.profdata" "$builddir/glifistore_tests" \
    > "$outdir/coverage.raw.lcov"
else
  lcov --capture --directory "$builddir" --output-file "$outdir/coverage.raw.lcov" \
    --ignore-errors mismatch,inconsistent,unused
fi
lcov --remove "$outdir/coverage.raw.lcov" \
  '/usr/*' '*/tests/*' '*/_deps/*' \
  --output-file "$outdir/coverage.lcov" \
  --ignore-errors inconsistent,unused \
  | tee "$outdir/coverage-report.txt"
[[ -s "$outdir/coverage.raw.lcov" && -s "$outdir/coverage.lcov" &&
   -s "$outdir/coverage-report.txt" ]]
grep -q '^SF:' "$outdir/coverage.raw.lcov"
grep -q '^DA:' "$outdir/coverage.raw.lcov"
grep -q '^SF:' "$outdir/coverage.lcov"
grep -q '^DA:' "$outdir/coverage.lcov"

echo "Coverage artifacts under $outdir (diagnostic only; not an acceptance gate)."
ls -la "$outdir"
