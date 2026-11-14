#!/usr/bin/env bash
# Thin wrapper: run the install stage of a packaging backend through scripts/package-ci.sh.
# All packaging logic lives in package-ci.sh and the backend adapters it dispatches to.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$root/scripts/package-ci.sh" --stage install "$@"
