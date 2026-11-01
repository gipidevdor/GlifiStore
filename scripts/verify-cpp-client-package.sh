#!/usr/bin/env bash
# Verify the C++ TCP client is installable via CMake package config.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected="$(tr -d '[:space:]' <"$root/VERSION")"

prefer=(
  "$root/build/macos-native-release"
  "$root/build/macos-release"
  "$root/build/unix-release"
  "$root/build/macos-debug"
  "$root/build/unix-debug"
)

build_dir="${GLIFISTORE_BUILD:-}"
if [[ -z "$build_dir" ]]; then
  for dir in "${prefer[@]}"; do
    if [[ -f "$dir/CMakeCache.txt" ]]; then
      build_dir="$dir"
      break
    fi
  done
fi
if [[ -z "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "missing configured CMake build (set GLIFISTORE_BUILD=...)" >&2
  exit 1
fi

cmake_bin="${CMAKE:-}"
if [[ -z "$cmake_bin" ]]; then
  for pyver in python3.14 python3.13; do
    candidate="$root/.tools/venv/lib/${pyver}/site-packages/cmake/data/bin/cmake"
    if [[ -x "$candidate" ]]; then
      cmake_bin="$candidate"
      break
    fi
  done
  if [[ -z "$cmake_bin" ]]; then
    cmake_bin="$(command -v cmake)"
  fi
fi
ctest_bin="${CTEST:-$(dirname "$cmake_bin")/ctest}"
if [[ ! -x "$ctest_bin" ]]; then
  ctest_bin="$(command -v ctest)"
fi

prefix="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-cpp-prefix.XXXXXX")"
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-cpp-consumer.XXXXXX")"
cleanup() { rm -rf "$prefix" "$work"; }
trap cleanup EXIT

"$cmake_bin" --build "$build_dir" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)" \
  --target glifistore_core glifistore_wire glifistore_client 2>/dev/null \
  || "$cmake_bin" --build "$build_dir" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)"

"$cmake_bin" --install "$build_dir" --prefix "$prefix"

# Prefer the existing consumer smoke test when present.
if [[ -d "$root/tests/consumer" ]]; then
  "$cmake_bin" -S "$root/tests/consumer" -B "$work/build" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_BUILD_TYPE=Release
  "$cmake_bin" --build "$work/build" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)"
  "$ctest_bin" --test-dir "$work/build" --output-on-failure
else
  cat >"$work/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(glifistore_client_pkg_check LANGUAGES CXX)
find_package(GlifiStore $expected REQUIRED CONFIG)
add_executable(check_client check.cpp)
target_link_libraries(check_client PRIVATE GlifiStore::client)
EOF
  cat >"$work/check.cpp" <<'EOF'
#include <glifistore/client/client.hpp>
int main() { return 0; }
EOF
  "$cmake_bin" -S "$work" -B "$work/build" -DCMAKE_PREFIX_PATH="$prefix"
  "$cmake_bin" --build "$work/build"
fi

mkdir -p "$root/sdk/cpp-dist"
{
  echo "cmake_package=GlifiStore"
  echo "target=GlifiStore::client"
  echo "version=$expected"
  echo "prefix_smoke=$prefix"
  echo "note=C++ client ships via CMake install; not a language-registry tarball"
} >"$root/sdk/cpp-dist/package-info.txt"

echo "C++ client packaging verification OK (GlifiStore::client via CMake)"
