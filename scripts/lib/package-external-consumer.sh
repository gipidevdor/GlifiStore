#!/usr/bin/env bash
# Build packaging/common/consumer against an installed GlifiStore prefix.
#
# Usage: package-external-consumer.sh <repo-root> <install-prefix> <work-directory>
#
# The consumer sources are copied outside the checkout; cmake/ctest run against
# CMAKE_PREFIX_PATH only. assert_consumer_isolation.py refuses any include or
# link path that resolves back into the repository. The caller retains stdout
# (and typically appends the FREEBSD/OPENBSD-PACKAGE external-consumer PASSED
# marker) as the evidence log.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <repo-root> <install-prefix> <work-directory>" >&2
  exit 2
fi

root="$(cd "$1" && pwd -P)"
prefix="$(cd "$2" && pwd -P)"
work="$(mkdir -p "$3" && cd "$3" && pwd -P)"

consumer_src="$root/packaging/common/consumer"
[[ -f "$consumer_src/CMakeLists.txt" ]] || {
  echo "error: packaged consumer sources missing at $consumer_src" >&2
  exit 1
}
command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake is required for the packaged external consumer" >&2
  exit 1
}
command -v ctest >/dev/null 2>&1 || {
  echo "error: ctest is required for the packaged external consumer" >&2
  exit 1
}

source_dir="$work/external-consumer/src"
build_dir="$work/external-consumer/build"
rm -rf "$work/external-consumer"
mkdir -p "$source_dir"
cp -R "$consumer_src/." "$source_dir/"

# Drop checkout-aware search paths so the consumer cannot silently fall back to
# the tree it was copied from.
unset CMAKE_PREFIX_PATH CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH || true
export CMAKE_PREFIX_PATH="$prefix"

generator_args=()
if command -v ninja >/dev/null 2>&1; then
  generator_args=(-GNinja)
fi

cmake -S "$source_dir" -B "$build_dir" "${generator_args[@]}" \
  "-DCMAKE_PREFIX_PATH=$prefix" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
python3 "$root/engineering/tools/assert_consumer_isolation.py" \
  --compile-commands "$build_dir/compile_commands.json" \
  --forbidden-root "$root"
