#!/usr/bin/env bash
# Unit checks for scripts/lib/linux_cpu_lists.sh (no hardware required).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../lib/linux_cpu_lists.sh
source "$root/scripts/lib/linux_cpu_lists.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

got="$(expand_cpu_list '0-3,8,10-11' | tr '\n' ' ')"
[[ "$got" == "0 1 2 3 8 10 11 " ]] || fail "expand got '$got'"

count="$(count_cpu_list '0-3,8')"
[[ "$count" == "5" ]] || fail "count got $count"

cpu_lists_disjoint '0-3' '4-7' || fail "expected disjoint"
if cpu_lists_disjoint '0-3' '3-5' 2>/dev/null; then
  fail "expected overlap"
fi

cpu_list_subset '1,2' '0-3' || fail "expected subset"
if cpu_list_subset '1,9' '0-3' 2>/dev/null; then
  fail "expected non-subset"
fi

if expand_cpu_list '' >/dev/null 2>&1; then
  fail "empty list should fail"
fi
if expand_cpu_list 'a-b' >/dev/null 2>&1; then
  fail "malformed list should fail"
fi

echo "ok scripts/tests/test_linux_cpu_lists.sh"
