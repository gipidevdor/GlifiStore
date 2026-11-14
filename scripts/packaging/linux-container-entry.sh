#!/bin/sh
# Entry point for the digest-pinned container that runs the Linux packaging lifecycle.
#
# The base images are minimal and do not ship Python, so this script cannot be
# Python and cannot assume anything beyond a POSIX shell and the distribution
# package manager. It installs just enough to reach the driver and then hands
# over; every status decision belongs to
# engineering/tools/run_linux_package_backend.py.
#
# Mount contract, established by that driver:
#   /src                   repository, read-only
#   /out                   evidence directory, writable
#   /release-context.json  release context, read-only
#   /candidate             sealed release candidate, read-only (optional)
#   HOST_UID / HOST_GID    host identities used to chown /out before exit so the
#                          outer (non-root) runner can read evidence and write
#                          fallback check plans after the container returns.
set -eu

backend=""
profile=""
stage="full"

while [ $# -gt 0 ]; do
    case "$1" in
        --backend) backend="$2"; shift 2 ;;
        --profile) profile="$2"; shift 2 ;;
        --stage) stage="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$backend" in
    deb|rpm) ;;
    *) echo "error: --backend must be deb or rpm" >&2; exit 2 ;;
esac
[ -n "$profile" ] || { echo "error: --profile is required" >&2; exit 2; }
[ -d /src ] || { echo "error: the repository is not mounted at /src" >&2; exit 2; }
[ -d /out ] || { echo "error: the evidence directory is not mounted at /out" >&2; exit 2; }
[ -f /release-context.json ] || { echo "error: /release-context.json is not mounted" >&2; exit 2; }

# Restore host ownership even when the driver fails or is killed: without this,
# root-owned files under /out become Permission denied on the outer runner and
# turn into a false packaging FAIL.
restore_out_ownership() {
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "${HOST_UID}:${HOST_GID}" /out 2>/dev/null || true
    fi
}
trap restore_out_ownership EXIT

# Only Python itself is bootstrapped here. The driver installs the packaging
# toolchain, because it has to report a failure to do so as BLOCKED evidence
# rather than as a container that died without saying anything.
if ! command -v python3 >/dev/null 2>&1; then
    if [ "$backend" = deb ]; then
        DEBIAN_FRONTEND=noninteractive apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            python3 python3-yaml python3-jsonschema
    else
        dnf install -y python3 python3-pyyaml python3-jsonschema
    fi
fi

status=0
python3 /src/engineering/tools/run_linux_package_backend.py \
    --backend "$backend" --profile "$profile" --stage "$stage" \
    --root /src --release-context /release-context.json \
    --output-dir /out --work-dir /glifistore-work --inner \
    || status=$?
exit "$status"
