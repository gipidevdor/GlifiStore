#!/usr/bin/env bash
# Assert every official SDK reports the same version as the repository VERSION file.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected="$(tr -d '[:space:]' <"$root/VERSION")"
if [[ ! "$expected" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
  echo "VERSION file is not a semver-like string: '$expected'" >&2
  exit 1
fi

fail=0
check() {
  local label="$1" actual="$2"
  if [[ "$actual" != "$expected" ]]; then
    echo "version mismatch: $label='$actual' expected='$expected'" >&2
    fail=1
  else
    echo "OK $label=$actual"
  fi
}

check "root/VERSION" "$expected"

py_ver="$(
  PYTHONPATH="$root/sdk/python/src${PYTHONPATH:+:$PYTHONPATH}" \
    "${PYTHON:-python3}" -c 'import glifistore; print(glifistore.__version__)'
)"
check "python/glifistore.__version__" "$py_ver"

perl_versions="$(
  { grep -RhoE "our \\\$VERSION = '[^']+'" "$root/sdk/perl/lib" || true; } |
    sed -E "s/.*our \\\$VERSION = '([^']+)'.*/\\1/" |
    sort -u
)"
perl_count="$(printf '%s\n' "$perl_versions" | grep -c . || true)"
if [[ "$perl_count" -ne 1 ]]; then
  echo "perl VERSION drift across modules: ${perl_versions:-<none>}" >&2
  fail=1
  perl_ver="<drift>"
else
  perl_ver="$perl_versions"
fi
check "perl/VERSION (all modules)" "$perl_ver"

go_ver="$("${GO:-go}" -C "$root/sdk/go" run ./cmd/glifistore-version)"
check "go/client.Version" "$go_ver"

ruby_ver="$(
  sed -nE 's/^[[:space:]]*VERSION[[:space:]]*=[[:space:]]*"([^"]+)"[[:space:]]*$/\1/p' \
    "$root/sdk/ruby/lib/glifi_store/version.rb"
)"
check "ruby/GlifiStore::VERSION" "$ruby_ver"

erlang_ver="$(
  sed -nE 's/^[[:space:]]*<<"([^"]+)">>\.[[:space:]]*$/\1/p' \
    "$root/sdk/erlang/src/glifistore_version.erl"
)"
erlang_app_ver="$(
  sed -nE 's/^[[:space:]]*\{vsn,[[:space:]]*"([^"]+)"\},[[:space:]]*$/\1/p' \
    "$root/sdk/erlang/src/glifistore.app.src"
)"
check "erlang/glifistore_version" "$erlang_ver"
check "erlang/application.vsn" "$erlang_app_ver"

check "cmake/PROJECT_VERSION (VERSION file)" "$expected"

if [[ "$fail" -ne 0 ]]; then
  echo "SDK versions are not locked to $expected" >&2
  exit 1
fi
echo "All official SDK versions match $expected"
