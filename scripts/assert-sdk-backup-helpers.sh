#!/usr/bin/env bash
# Fail closed if any official SDK lacks a typed backup API for wire BACKUP.
# Usage: scripts/assert-sdk-backup-helpers.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

fail=0

require_match() {
  local label="$1"
  local pattern="$2"
  local file="$3"
  if [[ ! -f "$file" ]]; then
    echo "FAIL: $label — missing file: $file" >&2
    fail=1
    return
  fi
  if ! grep -Eq "$pattern" "$file"; then
    echo "FAIL: $label — pattern /$pattern/ not found in $file" >&2
    fail=1
    return
  fi
  echo "ok: $label ($file)"
}

# C++
require_match "C++ Client::backup" \
  'Client::backup|auto backup\(' \
  "include/glifistore/client/client.hpp"

# Python
require_match "Python sync backup" \
  'def backup' \
  "sdk/python/src/glifistore/client.py"
require_match "Python async backup" \
  'def backup' \
  "sdk/python/src/glifistore/async_client.py"

# Go — method Backup on a receiver
if ! find sdk/go -type f \( -name '*.go' ! -name '*_test.go' \) -print0 \
  | xargs -0 grep -Elq 'func \(.*\) Backup'; then
  echo "FAIL: Go Backup — no 'func (.*) Backup' in sdk/go non-test sources" >&2
  fail=1
else
  echo "ok: Go Backup (sdk/go)"
fi

# Perl
require_match "Perl backup" \
  'sub backup' \
  "sdk/perl/lib/GlifiStore/Client.pm"

# Ruby
require_match "Ruby sync backup" \
  'def backup' \
  "sdk/ruby/lib/glifi_store/client.rb"
require_match "Ruby async backup" \
  'def backup' \
  "sdk/ruby/lib/glifi_store/async_client.rb"

# Erlang — export or function clause
erl="sdk/erlang/src/glifistore_client.erl"
if [[ ! -f "$erl" ]]; then
  echo "FAIL: Erlang backup — missing file: $erl" >&2
  fail=1
elif ! grep -Eq 'backup\(' "$erl"; then
  echo "FAIL: Erlang backup — no backup( in $erl" >&2
  fail=1
else
  echo "ok: Erlang backup ($erl)"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "assert-sdk-backup-helpers: FAILED" >&2
  exit 1
fi

echo "assert-sdk-backup-helpers: all official SDK typed backup helpers present"
echo "note: runtime BACKUP smoke is scripts/test-sdk-backup-interop.sh"
