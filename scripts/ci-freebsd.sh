#!/usr/bin/env bash
# FreeBSD native CI gate (build + test).
#
# Proves the kqueue / posix_fallocate / fdatasync code paths compile and pass the
# general suite on FreeBSD. This is a portability/regression signal, not UFS/ZFS
# durability certification (E3/E4 remain open).
#
# Intended for:
#   - GitHub Actions via vmactions/freebsd-vm
#   - Native FreeBSD developer hosts
#
# Environment:
#   GLIFISTORE_FREEBSD_PRESET   cmake preset (default: unix-release)
#   GLIFISTORE_FREEBSD_FUZZ     set to 1 to also configure/build unix-fuzz (no run)
#   CC / CXX                     optional compiler overrides
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${GLIFISTORE_FREEBSD_PRESET:-unix-release}"
build_dir="$root/build/${preset}"

if [[ "$(uname -s)" != "FreeBSD" ]]; then
  echo "error: this gate must run on FreeBSD (got $(uname -s))" >&2
  echo "hint: use .github/workflows/freebsd.yml or a native FreeBSD host" >&2
  exit 1
fi

echo "== FreeBSD CI gate =="
echo "os=$(uname -a)"
echo "freebsd_version=$(freebsd-version 2>/dev/null || true)"
echo "cwd=$root"
echo "preset=$preset"

command -v cmake >/dev/null 2>&1 || { echo "error: cmake required (pkg install cmake)" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "error: ninja required (pkg install ninja)" >&2; exit 1; }

# Prefer an explicit Clang when present; FreeBSD base clang is normally enough.
if [[ -z "${CC:-}" ]] && command -v clang >/dev/null 2>&1; then
  export CC=clang
fi
if [[ -z "${CXX:-}" ]] && command -v clang++ >/dev/null 2>&1; then
  export CXX=clang++
fi

echo "== toolchain =="
echo "CC=${CC:-"(default)"} CXX=${CXX:-"(default)"}"
cmake --version | head -1
"${CXX:-c++}" --version | head -1 || true

echo "== configure =="
rm -rf "$build_dir"
# AUTO TLS: enable when OpenSSL 3.x is installed via pkg; otherwise cleartext-only.
cmake --preset "$preset" | tee /tmp/glifistore-freebsd-cmake.log

echo "== build =="
cmake --build --preset "$preset"

echo "== ctest =="
# Crash / fault-injection harnesses are slow and timeout-prone under FreeBSD qemu
# VMs (600s+). Keep the portability signal on the general suite + benchmark smokes;
# Linux Sanitizers / Durability evidence remain the crash-lab authority.
ctest --preset "$preset" --output-on-failure -LE 'crash|fault-injection'

echo "== installed C/C++ consumers =="
install_root="$(mktemp -d /tmp/glifistore-freebsd-install.XXXXXX)"
consumer_build="$(mktemp -d /tmp/glifistore-freebsd-consumer.XXXXXX)"
cleanup_install() {
  rm -rf "$install_root" "$consumer_build"
}
trap cleanup_install EXIT
cmake --install "$build_dir" --prefix "$install_root"
test -x "$install_root/bin/glifistored"
test -f "$install_root/include/glifistore/abi/glifistore.h"
find "$install_root/lib" -maxdepth 1 -name 'libglifistore.so.[0-9]*' -type f | grep -q .

cmake -S tests/consumer -B "$consumer_build" -G Ninja \
  -DCMAKE_PREFIX_PATH="$install_root"
cmake --build "$consumer_build"
LD_LIBRARY_PATH="$install_root/lib" \
  ctest --test-dir "$consumer_build" --output-on-failure

PKG_CONFIG_PATH="$install_root/libdata/pkgconfig:$install_root/lib/pkgconfig" \
  pkg-config --cflags --libs glifistore-abi >"$consumer_build/abi.flags"
# shellcheck disable=SC2046
cc -std=c11 tests/consumer/abi.c \
  $(cat "$consumer_build/abi.flags") -o "$consumer_build/pkgconfig-abi-consumer"
LD_LIBRARY_PATH="$install_root/lib" "$consumer_build/pkgconfig-abi-consumer"

if [[ "${GLIFISTORE_FREEBSD_FUZZ:-0}" == "1" ]]; then
  echo "== fuzz build (compile only; continuous run stays on Linux Sanitizers) =="
  export CC="${CC:-clang}"
  export CXX="${CXX:-clang++}"
  rm -rf "$root/build/unix-fuzz"
  cmake --preset unix-fuzz
  cmake --build --preset unix-fuzz
fi

echo "FreeBSD CI gate OK"
echo "note: installed-prefix ABI proof is not a native ports package/service-lifecycle proof"
echo "note: this job does not pin UFS/ZFS or claim E3/E4 durability evidence"
