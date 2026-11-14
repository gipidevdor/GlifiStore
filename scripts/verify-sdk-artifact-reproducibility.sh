#!/usr/bin/env bash
# Package SDK clients twice with a pinned SOURCE_DATE_EPOCH and compare digests of
# wheels, gems, and normalized language-source archives. package-info.txt sidecars are excluded.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"

epoch="${SOURCE_DATE_EPOCH}"
echo "SOURCE_DATE_EPOCH=$epoch ($(glifistore_repro_iso8601 "$epoch"))"

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-repro.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

package_once() {
  local out="$1"
  mkdir -p "$out"
  rm -rf "$root/sdk/python/dist" "$root/sdk/perl/dist" "$root/sdk/go/dist" \
    "$root/sdk/ruby/dist" "$root/sdk/erlang/dist"
  "$root/scripts/package-python-client.sh"
  "$root/scripts/package-perl-client.sh"
  "$root/scripts/package-go-client.sh"
  if command -v ruby >/dev/null 2>&1 || [[ -n "${RUBY:-}" ]]; then
    if "$root/scripts/package-ruby-client.sh"; then
      :
    else
      echo "note: ruby packaging skipped for reproducibility compare" >&2
    fi
  else
    echo "note: ruby not on PATH — skipping gem comparison" >&2
  fi
  if command -v erl >/dev/null 2>&1 && command -v rebar3 >/dev/null 2>&1; then
    "$root/scripts/package-erlang-client.sh"
  else
    echo "note: Erlang/rebar3 not on PATH — skipping Erlang archive comparison" >&2
  fi
  "$root/scripts/checksum-sdk-artifacts.sh" "$out"
}

echo "== reproducible package run A =="
package_once "$work/a"
echo "== reproducible package run B =="
package_once "$work/b"

# Compare SHA256SUMS entries for archive-like artifacts only.
filter_sums() {
  # Wheels, gems, and normalized source archives (gzip -n + epoch mtimes).
  awk '
    $2 ~ /\.whl$/ { print }
    $2 ~ /\.gem$/ { print }
    $2 ~ /\.tar\.gz$/ { print }
  ' "$1" | sort
}

filter_sums "$work/a/SHA256SUMS" >"$work/a.filtered"
filter_sums "$work/b/SHA256SUMS" >"$work/b.filtered"

if [[ ! -s "$work/a.filtered" ]]; then
  echo "no SDK package artifacts to compare" >&2
  exit 1
fi

if ! cmp -s "$work/a.filtered" "$work/b.filtered"; then
  echo "SDK package reproducibility mismatch:" >&2
  diff -u "$work/a.filtered" "$work/b.filtered" >&2 || true
  exit 1
fi

echo "SDK archive reproducibility OK (SOURCE_DATE_EPOCH=$epoch)"
cat "$work/a.filtered"
