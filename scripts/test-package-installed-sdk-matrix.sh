#!/usr/bin/env bash
# Cross-SDK post-install matrix against a package-installed GlifiStore daemon.
#
# scripts/test-secure-profile-installed-artifacts.sh already proves the
# C++/Python/Perl/Ruby/Go/Erlang SDKs against a distributed daemon. This wrapper
# adds the one property packaging CI needs and cannot infer: the daemon under
# test must be a file the package manager owns. A daemon taken from the build
# tree, or a prefix the package inventory does not list, can never be reported
# as a packaged-daemon proof.
#
# Inputs (environment):
#   GLIFISTORE_PACKAGE_DAEMON    installed glifistored path (default: $GLIFISTORED)
#   GLIFISTORE_PACKAGE_FILE_LIST file inventory owned by the package manager,
#                                 one absolute path per line, as produced by
#                                 dpkg -L / rpm -ql / pkg list / pkg_info -L /
#                                 port contents / brew list
#   GLIFISTORE_PACKAGE_PREFIX    installed prefix (default: dirname of the daemon)
#   INSTALLED_INTEROP_PROFILE     secure (default) or plain
#
# The report written to --report is the only claim this script makes. Missing
# prerequisites are NOT_RUN, an unprovable package ownership is BLOCKED, and only
# a delegated run that actually passed is PASS.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
report=""
replace=0

usage() {
  cat >&2 <<'USAGE'
usage: test-package-installed-sdk-matrix.sh --report FILE [--replace]
USAGE
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --report) [[ $# -ge 2 ]] || usage; report="$2"; shift 2 ;;
    --replace) replace=1; shift ;;
    -h|--help) usage ;;
    *) echo "error: unknown argument: $1" >&2; usage ;;
  esac
done
[[ -n "$report" ]] || usage
command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 1; }

daemon="${GLIFISTORE_PACKAGE_DAEMON:-${GLIFISTORED:-}}"
file_list="${GLIFISTORE_PACKAGE_FILE_LIST:-}"
prefix="${GLIFISTORE_PACKAGE_PREFIX:-}"
languages=""

# Writes the report through the schema, so a malformed or over-claiming report
# cannot be produced at all.
write_report() {
  local result="$1" installed="$2" reason="$3"
  python3 - "$report" "$result" "$installed" "$daemon" "$prefix" "$file_list" \
    "$languages" "$reason" "$root" "$replace" <<'PY'
import sys
from pathlib import Path

(
    report,
    result,
    installed,
    daemon,
    prefix,
    file_list,
    languages,
    reason,
    root,
    replace,
) = sys.argv[1:11]
sys.path.insert(0, root)

from engineering.tools.package_framework import (
    utc_now,
    validate_against_schema,
    write_json,
)

value = {
    "schema_version": 1,
    "generated_at": utc_now(),
    "result": result,
    "package_installed": installed == "1",
    "daemon": daemon or None,
    "prefix": prefix or None,
    "file_list": file_list or None,
    "languages": [name for name in languages.split(",") if name],
    "reason": reason,
}
validate_against_schema(value, "installed-sdk-matrix.schema.json", "installed SDK matrix report")
write_json(Path(report), value, replace=replace == "1")
print(f"installed SDK matrix {result}: {reason}")
PY
}

not_run() {
  write_report NOT_RUN 0 "$1"
  exit 0
}

blocked() {
  write_report BLOCKED 0 "$1"
  exit 1
}

if [[ -z "$daemon" || -z "$file_list" ]]; then
  not_run "GLIFISTORE_PACKAGE_DAEMON and GLIFISTORE_PACKAGE_FILE_LIST are required to claim a package-installed daemon; neither is inferred"
fi
[[ -x "$daemon" ]] || blocked "the declared packaged daemon is not executable: $daemon"
[[ -f "$file_list" && ! -L "$file_list" ]] || blocked "the package file inventory is missing or not a regular file: $file_list"

# Both spellings are kept: the prefix and checkout tests need the canonical path,
# while a package inventory records the path as the package manager installed it,
# which may traverse a symlinked prefix component.
declared="$daemon"
[[ "$declared" == /* ]] || declared="$(pwd -P)/$declared"
daemon="$(cd "$(dirname "$daemon")" && pwd -P)/$(basename "$daemon")"
[[ -n "$prefix" ]] || prefix="$(cd "$(dirname "$daemon")/.." && pwd -P)"
[[ -d "$prefix" ]] || blocked "the declared installed prefix is not a directory: $prefix"
prefix="$(cd "$prefix" && pwd -P)"

# A daemon inside the checkout is a build-tree daemon whatever the inventory says.
case "$daemon" in
  "$root"/*) blocked "the daemon under test lives inside the source checkout: $daemon" ;;
esac
case "$daemon" in
  "$prefix"/*) ;;
  *) blocked "the daemon is outside the declared installed prefix: $daemon" ;;
esac
if ! grep -Fxq "$daemon" "$file_list" && ! grep -Fxq "$declared" "$file_list"; then
  blocked "the package file inventory does not own $daemon, so no packaged-daemon claim is possible"
fi

# The delegated matrix needs the sealed SDK distribution archives; without them
# nothing ran and the honest answer is NOT_RUN, not FAIL.
shopt -s nullglob
missing=()
for name in python perl ruby go erlang; do
  case "$name" in
    python) found=("$root/sdk/python/dist/glifistore-"*.whl) ;;
    perl) found=("$root/sdk/perl/dist/GlifiStore-"*.tar.gz) ;;
    ruby) found=("$root/sdk/ruby/dist/glifistore-"*.gem) ;;
    go) found=("$root/sdk/go/dist/glifistore-go-"*.tar.gz) ;;
    erlang) found=("$root/sdk/erlang/dist/glifistore-erlang-"*.tar.gz) ;;
  esac
  (( ${#found[@]} == 1 )) || missing+=("$name")
done
shopt -u nullglob
if (( ${#missing[@]} > 0 )); then
  # Ownership was proven; missing sealed SDK archives mean the matrix did not run,
  # not that the daemon was unpackaged.
  write_report NOT_RUN 1 "no sealed SDK distribution archive for: ${missing[*]}"
  exit 0
fi

profile="${INSTALLED_INTEROP_PROFILE:-secure}"
log="$(mktemp "${TMPDIR:-/tmp}/glifistore-installed-sdk-matrix.XXXXXX")"
status=0
env \
  GLIFISTORED="$daemon" \
  GLIFISTORE_CPP_PREFIX="$prefix" \
  INSTALLED_INTEROP_PROFILE="$profile" \
  "$root/scripts/test-secure-profile-installed-artifacts.sh" >"$log" 2>&1 || status=$?
cat "$log"
if (( status != 0 )); then
  write_report FAIL 1 "the $profile cross-SDK matrix failed against the packaged daemon; see $log"
  exit 1
fi
languages="cpp,python,go,perl,ruby,erlang"
write_report PASS 1 "the $profile cross-SDK matrix passed against the package-owned daemon $daemon"
