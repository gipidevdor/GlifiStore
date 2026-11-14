#!/usr/bin/env bash
# Shared sealed N-1 package selection for FreeBSD/OpenBSD lifecycle scripts.
# Never rebuilds N-1 from HEAD.

# Print the SemVer predecessor version from GLIFISTORE_RELEASE_CONTEXT, or an
# empty line when none is available. Fails when the N-1 directory is set without
# a release context.
bsd_n1_previous_version() {
  local context_path="${GLIFISTORE_RELEASE_CONTEXT:-}"
  local n1_dir="${GLIFISTORE_N1_PACKAGE_DIR:-}"
  [[ -n "$n1_dir" ]] || {
    echo ""
    return 0
  }
  [[ -n "$context_path" && -f "$context_path" ]] || {
    echo "error: GLIFISTORE_N1_PACKAGE_DIR requires GLIFISTORE_RELEASE_CONTEXT" >&2
    return 1
  }
  python3 - "$context_path" <<'PY'
import json
import os
import sys

context = json.loads(open(sys.argv[1], encoding="utf-8").read())
previous = context.get("previous") or {}
if not previous.get("available"):
    if os.environ.get("GLIFISTORE_N1_PACKAGE_DIR", "").strip():
        raise SystemExit(
            "GLIFISTORE_N1_PACKAGE_DIR is set but the release context has no SemVer predecessor"
        )
    print("")
    raise SystemExit(0)
version = previous.get("version") or ""
if not version:
    raise SystemExit("previous release is available but carries no version")
print(version)
PY
}

# Print sealed N-1 package paths for backend/version under GLIFISTORE_N1_PACKAGE_DIR.
bsd_n1_select_packages() {
  local backend="$1"
  local version="$2"
  local n1_dir="${GLIFISTORE_N1_PACKAGE_DIR:-}"
  [[ -n "$n1_dir" && -n "$version" ]] || return 1
  python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/engineering/tools/n1_package_artifacts.py" \
    select-bsd --backend "$backend" --directory "$n1_dir" --version "$version"
}
