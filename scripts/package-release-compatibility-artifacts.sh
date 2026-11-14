#!/usr/bin/env bash
# Package in-tree golden fixtures as a labeled released-compatibility tree.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <label>" >&2
  echo "example: $0 0.1.0-alpha.1" >&2
  exit 2
fi

label="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
source_fixtures="${root}/tests/fixtures"
destination="${source_fixtures}/released/${label}"

if [[ "${#label}" -gt 128 || ! "$label" =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ || "$label" == *..* ]]; then
  echo "label must be a safe flat release identifier (got '$label')" >&2
  exit 2
fi
if ! mkdir "${destination}" 2>/dev/null; then
  echo "released fixture label already exists: ${destination}" >&2
  exit 1
fi
complete=0
cleanup() {
  if [[ "$complete" != "1" ]]; then
    rm -rf "${destination}"
  fi
}
trap cleanup EXIT

shopt -s nullglob
copied=0
for fixture in "${source_fixtures}"/*.hex; do
  cp "${fixture}" "${destination}/"
  copied=$((copied + 1))
done
shopt -u nullglob

{
  echo "schema_version=2"
  echo "label=${label}"
  echo "glifistore_version=$(tr -d '[:space:]' <"${root}/VERSION")"
  echo "persistence_format=1"
  echo "wire_protocol=2"
  echo "packaged_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  if git -C "${root}" rev-parse HEAD >/dev/null 2>&1; then
    echo "git_commit=$(git -C "${root}" rev-parse HEAD)"
    echo "git_describe=$(git -C "${root}" describe --always --dirty 2>/dev/null || true)"
  fi
  echo "fixture_count=${copied}"
} >"${destination}/METADATA.txt"

if command -v shasum >/dev/null 2>&1; then
  (
    cd "${destination}"
    shasum -a 256 *.hex | sort >SHA256SUMS
  )
elif command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${destination}"
    sha256sum *.hex | sort >SHA256SUMS
  )
else
  echo "need shasum or sha256sum to bind released fixtures" >&2
  exit 1
fi

python3 "${root}/engineering/tools/validate_compat_matrix.py" --root "${root}"
complete=1
echo "packaged ${copied} fixtures into ${destination}"
