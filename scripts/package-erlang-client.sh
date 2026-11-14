#!/usr/bin/env bash
# Verify Erlang SDK packaging readiness (fixtures, compile, CT, version lock, CLIs).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
sdk="$root/sdk/erlang"
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-erlang-pack.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

if ! command -v rebar3 >/dev/null 2>&1; then
  echo "rebar3 is required (macOS: sudo port install rebar3)" >&2
  exit 1
fi
if ! command -v erl >/dev/null 2>&1; then
  echo "Erlang/OTP is required (macOS: sudo port install erlang; OTP >= 25)" >&2
  exit 1
fi

for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$sdk/test/fixtures/$fixture"; then
    echo "vendored fixture drift: $fixture" >&2
    echo "run ./scripts/sync-sdk-fixtures.sh" >&2
    exit 1
  fi
done

(
  cd "$sdk"
  rebar3 compile
  rebar3 ct --cover=false
)

expected="$(tr -d '[:space:]' <"$root/VERSION")"
got="$(erl -noshell -pa "$sdk/_build/default/lib/glifistore/ebin" \
  -eval 'io:format("~s", [glifistore_version:version()]), halt().')"
if [[ "$got" != "$expected" ]]; then
  echo "erlang glifistore_version:version()='$got' does not match VERSION='$expected'" >&2
  exit 1
fi

chmod +x "$sdk/scripts/glifistore-interop.escript" "$sdk/scripts/glifistore-version.escript" \
  "$sdk/benchmarks/client_benchmark.escript"
escript "$sdk/scripts/glifistore-version.escript" >/dev/null

mkdir -p "$sdk/dist"
rm -f "$sdk/dist"/glifistore-erlang-*.tar.gz
{
  echo "package=glifistore"
  echo "version=$got"
  echo "otp=$(erl -noshell -eval 'io:format("~s", [erlang:system_info(otp_release)]), halt().')"
  echo "rebar3=$(rebar3 version | head -1)"
  echo "source_date_epoch=$SOURCE_DATE_EPOCH"
  echo "built_at=$(glifistore_repro_iso8601)"
} >"$sdk/dist/package-info.txt"

for required in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
  if [[ ! -f "$sdk/$required" ]]; then
    echo "ERROR: Erlang SDK missing $required for Hex redistribution" >&2
    exit 1
  fi
done
if ! grep -q 'LICENSE' "$sdk/rebar.config" || ! grep -q 'NOTICE' "$sdk/rebar.config"; then
  echo "ERROR: rebar.config hex.extra_files must list LICENSE and NOTICE" >&2
  exit 1
fi

# Build a reproducible, tracked-source archive without claiming that it is a published Hex package.
archive_name="glifistore-erlang-$got.tar.gz"
archive_root="$work/glifistore-erlang-$got"
mkdir -p "$archive_root"
while IFS= read -r -d '' path; do
  relative="${path#sdk/erlang/}"
  mkdir -p "$archive_root/$(dirname "$relative")"
  cp "$root/$path" "$archive_root/$relative"
done < <(git -C "$root" ls-files -z -- sdk/erlang)
tar -czf "$sdk/dist/$archive_name" -C "$work" "$(basename "$archive_root")"
"$root/scripts/normalize-tar-gz.sh" "$sdk/dist/$archive_name"

# Extract and compile from outside the checkout so repository _build state cannot satisfy the proof.
verify_root="$work/verify"
mkdir -p "$verify_root"
tar -xzf "$sdk/dist/$archive_name" -C "$verify_root"
(
  cd "$verify_root/$(basename "$archive_root")"
  rebar3 compile >/dev/null
)
artifact_ebin="$verify_root/$(basename "$archive_root")/_build/default/lib/glifistore/ebin"
artifact_version="$(erl -noshell -pa "$artifact_ebin" \
  -eval 'io:format("~s", [glifistore_version:version()]), halt().')"
if [[ "$artifact_version" != "$got" ]]; then
  echo "Erlang source archive version '$artifact_version' does not match '$got'" >&2
  exit 1
fi

{
  echo "source_archive=$archive_name"
  echo "external_archive_compile=passed"
} >>"$sdk/dist/package-info.txt"

echo "Erlang packaging verification OK ($sdk/dist/$archive_name)"
echo "Publish path: Hex package glifistore@$got (see sdk/erlang/PACKAGING.md)"
