#!/usr/bin/env bash
# Export env vars that make SDK packaging more bit-stable across machines.
# Usage: source "$root/scripts/export-reproducible-build-env.sh"
#
# SOURCE_DATE_EPOCH defaults to the HEAD commit unix time (or keep an existing value).
# Callers that need an ISO-8601 stamp can use glifistore_repro_iso8601.

if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  if command -v git >/dev/null 2>&1 && git -C "${GLIFISTORE_ROOT:-.}" rev-parse HEAD >/dev/null 2>&1; then
    SOURCE_DATE_EPOCH="$(git -C "${GLIFISTORE_ROOT:-.}" log -1 --pretty=%ct)"
  else
    SOURCE_DATE_EPOCH=0
  fi
fi
export SOURCE_DATE_EPOCH
export TZ=UTC
export PYTHONHASHSEED="${PYTHONHASHSEED:-0}"
# Prefer UTC timestamps in Perl ExtUtils::MakeMaker / Archive::Tar when honored.
export PERL_HASH_SEED="${PERL_HASH_SEED:-0}"
export DETERMINISTIC_BUILD=1

glifistore_repro_iso8601() {
  local epoch="${1:-$SOURCE_DATE_EPOCH}"
  if date -u -d "@${epoch}" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null; then
    return 0
  fi
  date -u -r "${epoch}" +%Y-%m-%dT%H:%M:%SZ
}
