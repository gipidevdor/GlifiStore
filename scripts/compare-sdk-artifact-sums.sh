#!/usr/bin/env bash
# Compare two SHA256SUMS files for packaging archives (whl/gem/tar.gz).
# Usage: compare-sdk-artifact-sums.sh <reference.SHA256SUMS> <candidate.SHA256SUMS>
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <reference.SHA256SUMS> <candidate.SHA256SUMS>" >&2
  exit 2
fi
ref="$1"
cand="$2"
[[ -f "$ref" && -f "$cand" ]] || {
  echo "missing sums file" >&2
  exit 1
}

filter() {
  awk '
    $2 ~ /\.whl$/ { print }
    $2 ~ /\.gem$/ { print }
    $2 ~ /\.tar\.gz$/ { print }
  ' "$1" | sort
}

tmp="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-sumcmp.XXXXXX")"
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT

filter "$ref" >"$tmp/ref"
filter "$cand" >"$tmp/cand"

if [[ ! -s "$tmp/ref" ]]; then
  echo "reference SHA256SUMS has no whl/gem/tar.gz entries" >&2
  exit 1
fi

if ! cmp -s "$tmp/ref" "$tmp/cand"; then
  echo "cross-builder archive digest mismatch:" >&2
  diff -u "$tmp/ref" "$tmp/cand" >&2 || true
  exit 1
fi

echo "cross-builder archive digests match"
cat "$tmp/ref"
