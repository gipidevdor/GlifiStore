#!/usr/bin/env bash
# Run one ABI-v1 compatibility check without rebuilding either library.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
candidate_prefix=""
prior_prefix=""
prior_consumer=""
check=""
work=""

usage() {
  echo "usage: $0 --candidate-prefix DIR --prior-prefix DIR --prior-consumer FILE --work DIR --check CHECK" >&2
}

while (($#)); do
  case "$1" in
    --candidate-prefix) candidate_prefix="${2:-}"; shift 2 ;;
    --prior-prefix) prior_prefix="${2:-}"; shift 2 ;;
    --prior-consumer) prior_consumer="${2:-}"; shift 2 ;;
    --work) work="${2:-}"; shift 2 ;;
    --check) check="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done
case "$check" in
  exact-symbols|layout|old-consumer-new-library|new-consumer-old-library) ;;
  *) echo "invalid --check: $check" >&2; usage; exit 2 ;;
esac
if [[ -z "$candidate_prefix" || -z "$prior_prefix" || -z "$prior_consumer" || -z "$work" ]]; then
  usage
  exit 2
fi
candidate_prefix="$(cd "$candidate_prefix" && pwd -P)"
prior_prefix="$(cd "$prior_prefix" && pwd -P)"
mkdir -p "$work"
work="$(cd "$work" && pwd -P)"
if [[ ! -x "$prior_consumer" ]]; then
  echo "prior ABI consumer is not executable" >&2
  exit 1
fi
for prefix in "$candidate_prefix" "$prior_prefix"; do
  test -f "$prefix/include/glifistore/abi/glifistore.h"
  test -f "$prefix/share/GlifiStore/ABI_VERSION"
  test -e "$prefix/lib/libglifistore.so.1"
done
test "$(cut -d. -f1 "$candidate_prefix/share/GlifiStore/ABI_VERSION")" = "1"
test "$(cut -d. -f1 "$prior_prefix/share/GlifiStore/ABI_VERSION")" = "1"

compile_consumer() {
  local source="$1"
  local output="$2"
  PKG_CONFIG_PATH="$candidate_prefix/lib/pkgconfig" \
    cc -std=c11 "$source" $(PKG_CONFIG_PATH="$candidate_prefix/lib/pkgconfig" \
      pkg-config --cflags --libs glifistore-abi) -o "$output"
}

prove_library_resolution() {
  local binary="$1"
  local library_directory="$2"
  local resolution
  resolution="$(LD_LIBRARY_PATH="$library_directory" ldd "$binary")"
  printf '%s\n' "$resolution"
  grep -F "$library_directory/libglifistore.so.1" <<<"$resolution" >/dev/null
}

case "$check" in
  exact-symbols)
    library="$(readlink -f "$candidate_prefix/lib/libglifistore.so.1")"
    python3 "$root/engineering/tools/check_abi_symbols.py" \
      --library "$library" --allowlist "$root/abi/symbols-v1.txt"
    ;;
  layout)
    binary="$work/candidate-layout-probe"
    compile_consumer "$root/tests/abi/c_abi_layout_probe.c" "$binary"
    prove_library_resolution "$binary" "$candidate_prefix/lib"
    LD_LIBRARY_PATH="$candidate_prefix/lib" "$binary"
    ;;
  old-consumer-new-library)
    prove_library_resolution "$prior_consumer" "$candidate_prefix/lib"
    LD_LIBRARY_PATH="$candidate_prefix/lib" "$prior_consumer"
    ;;
  new-consumer-old-library)
    binary="$work/new-abi-consumer"
    compile_consumer "$root/tests/consumer/abi.c" "$binary"
    prove_library_resolution "$binary" "$prior_prefix/lib"
    LD_LIBRARY_PATH="$prior_prefix/lib" "$binary"
    ;;
esac

echo "ABI-COMPAT $check PASSED"
