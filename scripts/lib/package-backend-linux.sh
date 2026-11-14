#!/usr/bin/env bash
# Debian / RPM backend module for scripts/package-ci.sh (Wave B).
#
# Everything that decides a status lives in
# engineering/tools/run_linux_package_backend.py: it renders debian/ or the spec
# from the release context, chooses between running inside the digest-pinned
# container, running natively on a disposable host, or running nothing at all,
# and writes the check plan and the evidence itself.
#
# Both dispatch modes are opt-in because both are destructive or expensive:
#   GLIFISTORE_PACKAGE_CI_CONTAINER=1  build and install inside the pinned image
#   GLIFISTORE_PACKAGE_CI_NATIVE=1     build and install on this host, as root
# Without either, the backend reports the metadata rows it really resolved and
# BLOCKED or NOT_RUN for the rest.
#
# Sourced by package-ci.sh; uses its root, tools, profile, stage, output_dir,
# release_context and candidate_dir variables.

# Hand the sealed candidate to the driver when package-ci.sh was given one. The
# driver verifies the seal itself and refuses to build a release profile without it.
linux_backend_candidate() {
  local candidate="$1"
  [[ -n "$candidate" ]] || return 0
  GLIFISTORE_CANDIDATE_DIR="$candidate"
  export GLIFISTORE_CANDIDATE_DIR
}

linux_backend_run() {
  local backend="$1"
  local directory="$2"

  case "$backend" in
    deb|rpm) ;;
    *) echo "error: $backend is not a Linux packaging backend" >&2; return 2 ;;
  esac

  linux_backend_candidate "$candidate_dir"

  # The driver prints the PACKAGE-CI line and exits non-zero only on FAIL or on a
  # refusal; package-ci.sh turns that into the run status.
  python3 "$tools/run_linux_package_backend.py" --backend "$backend" --profile "$profile" \
    --stage "$stage" --root "$root" --release-context "$release_context" \
    --output-dir "$directory" --work-dir "$output_dir/$backend/work"
}
