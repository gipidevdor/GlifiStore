#!/usr/bin/env bash
# Package and verify every official language SDK, then checksum artifacts.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
echo "reproducible packaging SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

"$root/scripts/check-sdk-versions.sh"
"$root/scripts/sync-sdk-fixtures.sh"
"$root/scripts/package-python-client.sh"
"$root/scripts/package-perl-client.sh"
"$root/scripts/package-go-client.sh"
"$root/scripts/package-ruby-client.sh"
if command -v erl >/dev/null 2>&1 && command -v rebar3 >/dev/null 2>&1; then
  "$root/scripts/package-erlang-client.sh"
else
  echo "Erlang/OTP and rebar3 are required to package every official SDK" >&2
  exit 1
fi
"$root/scripts/verify-cpp-client-package.sh"
SDK_ARTIFACT_INDEX_REQUIRE_COMPLETE=1 "$root/scripts/checksum-sdk-artifacts.sh"

echo "All SDK packaging gates passed"
