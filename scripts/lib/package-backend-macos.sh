#!/usr/bin/env bash
# MacPorts / Homebrew backend module for scripts/package-ci.sh (Wave C).
#
# Everything that decides a status lives in
# engineering/tools/run_macos_package_backend.py: it renders the Portfile or the
# formula from the release context, walks the native lifecycle when the host can
# actually do it, and writes the check plan and the evidence itself.
#
# The native lifecycle installs into the host package manager, so it is opt-in
# through GLIFISTORE_PACKAGE_CI_NATIVE=1. Without it the backend reports the
# metadata rows it really resolved and NOT_RUN for the rest.
#
# The release profile takes its source from GLIFISTORE_SOURCE_ARCHIVE_URL and
# GLIFISTORE_SOURCE_ARCHIVE_SHA256; a checkout of HEAD is never admitted there.
#
# Sourced by package-ci.sh; uses its root, tools, profile, stage, output_dir,
# release_context and candidate_dir variables.

# Offer the sealed candidate source archive to the renderer when package-ci.sh was
# given one and the caller did not already pin a source. The digest is recomputed
# here; the renderer re-verifies it against the bytes it is told to pin.
macos_backend_candidate_source() {
  local candidate="$1"
  [[ -n "$candidate" ]] || return 0
  [[ -z "${GLIFISTORE_SOURCE_ARCHIVE_URL:-}" ]] || return 0
  local archive="$candidate/GlifiStore-$version.tar.xz"
  [[ -f "$archive" ]] || return 0
  GLIFISTORE_SOURCE_ARCHIVE_URL="file://$archive"
  GLIFISTORE_SOURCE_ARCHIVE_SHA256="$(python3 -c \
    'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' \
    "$archive")"
  export GLIFISTORE_SOURCE_ARCHIVE_URL GLIFISTORE_SOURCE_ARCHIVE_SHA256
}

macos_backend_run() {
  local backend="$1"
  local directory="$2"

  case "$backend" in
    macports|homebrew) ;;
    *) echo "error: $backend is not a macOS backend" >&2; return 2 ;;
  esac

  macos_backend_candidate_source "$candidate_dir"

  # The driver prints the PACKAGE-CI line and exits non-zero only on FAIL or on a
  # refusal; package-ci.sh turns that into the run status.
  python3 "$tools/run_macos_package_backend.py" --backend "$backend" --profile "$profile" \
    --stage "$stage" --root "$root" --release-context "$release_context" \
    --output-dir "$directory" --work-dir "$output_dir/$backend/work"
}
