#!/usr/bin/env bash
# Materialize a compact CI evidence bundle for the installed-SDK secure-profile matrix.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-}"
result="${2:-}"

if [[ -z "$out" || ("$result" != "passed" && "$result" != "failed") ]]; then
  echo "usage: $0 OUTPUT_DIR passed|failed" >&2
  exit 2
fi

mkdir -p "$out"
run_log="$out/run.log"
if [[ ! -s "$run_log" ]]; then
  echo "evidence requires a non-empty run.log" >&2
  exit 1
fi
if [[ "$result" == "passed" ]] && ! grep -Fqx \
  "Installed C++/Python/Perl/Ruby/Go/Erlang secure-profile interop PASSED" "$run_log"; then
  echo "passed evidence requires the installed-matrix success marker" >&2
  exit 1
fi

commit="${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}"
version="$(tr -d '[:space:]' <"$root/VERSION")"
created_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

{
  echo "schema_version=1"
  echo "evidence_kind=sdk-installed-secure-profile"
  echo "result=$result"
  echo "commit=$commit"
  echo "glifistore_version=$version"
  echo "wire_protocol=2"
  echo "created_utc=$created_utc"
  echo "runner_os=${RUNNER_OS:-$(uname -s)}"
  echo "runner_arch=${RUNNER_ARCH:-$(uname -m)}"
  echo "github_repository=${GITHUB_REPOSITORY:-local}"
  echo "github_run_id=${GITHUB_RUN_ID:-local}"
  echo "github_run_attempt=${GITHUB_RUN_ATTEMPT:-local}"
  echo "matrix=c++,python,perl,ruby,go,erlang"
  echo "profile=tls1.3,mtls,authz,keyed-routing,prefix,crl,quota"
  echo "run_log=run.log"
  echo "package_checksums=package-SHA256SUMS"
} >"$out/manifest.env"

checksums="$root/dist/sdk-artifacts/SHA256SUMS"
if [[ -f "$checksums" ]]; then
  cp "$checksums" "$out/package-SHA256SUMS"
else
  echo "packaged SDK checksum manifest missing: $checksums" >&2
  exit 1
fi
index="$root/dist/sdk-artifacts/sdk-release-index.json"
if [[ -f "$index" ]]; then
  python3 "$root/engineering/tools/write_sdk_release_index.py" \
    "$root/dist/sdk-artifacts" --verify-only --require-complete >/dev/null
  cp "$index" "$out/sdk-release-index.json"
else
  echo "SDK release index missing: $index" >&2
  exit 1
fi

{
  "${CMAKE:-cmake}" --version 2>/dev/null | head -n 1 || true
  "${CXX:-c++}" --version 2>/dev/null | head -n 1 || true
  "${PYTHON:-python3}" --version 2>&1 || true
  "${PERL:-perl}" -e 'printf "perl %vd\n", $^V' 2>/dev/null || true
  "${GO:-go}" version 2>/dev/null || true
  "${RUBY:-ruby}" --version 2>/dev/null || true
  erl -noshell -eval 'io:format("erlang OTP ~s~n", [erlang:system_info(otp_release)]), halt().' \
    2>/dev/null || true
  rebar3 version 2>/dev/null || true
} >"$out/toolchains.txt"

echo "SDK installed secure-profile evidence written to $out"
