#!/usr/bin/env bash
# Build the interop peer exclusively through an installed GlifiStore CMake package.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_root="${1:-}"
output="${2:-}"
cmake_bin="${CMAKE:-}"

if [[ -n "$cmake_bin" && ! -x "$cmake_bin" ]]; then
  cmake_bin="$(command -v "$cmake_bin" || true)"
elif [[ -z "$cmake_bin" ]]; then
  cmake_bin="$(command -v cmake || true)"
fi
if [[ -z "$cmake_bin" ]]; then
  for pyver in python3.14 python3.13; do
    candidate="$root/.tools/venv/lib/${pyver}/site-packages/cmake/data/bin/cmake"
    if [[ -x "$candidate" ]]; then
      cmake_bin="$candidate"
      break
    fi
  done
fi

if [[ -z "$artifact_root" || -z "$output" ]]; then
  echo "usage: $0 BUILD_DIR_OR_INSTALLED_PREFIX OUTPUT" >&2
  exit 2
fi
if [[ -z "$cmake_bin" || ! -x "$cmake_bin" ]]; then
  echo "CMake is required to build the installed C++ interop peer" >&2
  exit 1
fi

expected="$(tr -d '[:space:]' <"$root/VERSION")"
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-cpp-artifact.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
prefix="$work/prefix"
consumer="$work/consumer"
mkdir -p "$prefix" "$consumer"

if [[ -f "$artifact_root/CMakeCache.txt" ]]; then
  "$cmake_bin" --build "$artifact_root" --target glifistore_client glifistore_abi
  "$cmake_bin" --install "$artifact_root" --prefix "$prefix" --component AbiRuntime
  "$cmake_bin" --install "$artifact_root" --prefix "$prefix" --component Development
elif [[ -d "$artifact_root" ]]; then
  prefix="$(cd "$artifact_root" && pwd -P)"
else
  echo "first argument must be a configured build or installed prefix: $artifact_root" >&2
  exit 1
fi

configs=()
if command -v rg >/dev/null 2>&1; then
  while IFS= read -r config; do
    configs+=("$config")
  done < <(rg --files "$prefix" | rg '/GlifiStoreConfig\.cmake$')
else
  while IFS= read -r config; do
    configs+=("$config")
  done < <(find "$prefix" -type f -name GlifiStoreConfig.cmake -print)
fi
if [[ "${#configs[@]}" -ne 1 ]]; then
  echo "installed prefix must contain exactly one GlifiStoreConfig.cmake" >&2
  exit 1
fi
package_dir="$(dirname "${configs[0]}")"
for companion in GlifiStoreConfigVersion.cmake GlifiStoreTargets.cmake FindGlifiStoreTls.cmake; do
  if [[ ! -f "$package_dir/$companion" ]]; then
    echo "installed C++ package is missing $companion" >&2
    exit 1
  fi
done

cp "$root/tools/interop_client.cpp" "$consumer/interop_client.cpp"
cat >"$consumer/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(GlifiStoreInstalledInterop LANGUAGES CXX)
find_package(GlifiStore $expected EXACT REQUIRED CONFIG
  PATHS "\${GLIFISTORE_PACKAGE_DIR}"
  NO_DEFAULT_PATH)
add_executable(glifistore-interop-cpp interop_client.cpp)
target_link_libraries(glifistore-interop-cpp PRIVATE GlifiStore::client)
target_compile_features(glifistore-interop-cpp PRIVATE cxx_std_23)
if(DEFINED ENV{GLIFISTORE_SANITIZERS} AND NOT "\$ENV{GLIFISTORE_SANITIZERS}" STREQUAL "")
  target_compile_options(glifistore-interop-cpp PRIVATE -fno-omit-frame-pointer
    "-fsanitize=\$ENV{GLIFISTORE_SANITIZERS}")
  target_link_options(glifistore-interop-cpp PRIVATE
    "-fsanitize=\$ENV{GLIFISTORE_SANITIZERS}")
endif()
EOF

"$cmake_bin" -S "$consumer" -B "$consumer/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGLIFISTORE_PACKAGE_DIR="$package_dir" \
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
"$cmake_bin" --build "$consumer/build" --target glifistore-interop-cpp

built="$consumer/build/glifistore-interop-cpp"
if [[ ! -x "$built" ]]; then
  echo "installed C++ package did not produce glifistore-interop-cpp" >&2
  exit 1
fi
mkdir -p "$(dirname "$output")"
cp "$built" "$output"
"$output" --help >/dev/null 2>&1

echo "Installed C++ package interop build OK ($output)"
