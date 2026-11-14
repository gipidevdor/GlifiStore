#!/usr/bin/env bash
# Verify GitHub artifact attestations / SLSA provenance for packaged SDK artifacts.
#
# Honest private-repo semantics:
#   - Public repos, or private with ENABLE_ARTIFACT_ATTESTATIONS=true: verify when
#     ATTESTATIONS_REQUIRED=1 (CI sets this on tags that also run actions/attest).
#   - Private/internal without ENABLE_ARTIFACT_ATTESTATIONS: skip (exit 0) — GitHub.com
#     private attestations need GHEC + that opt-in variable.
#   - Missing gh / attestation tooling: skip unless ATTESTATIONS_REQUIRED=1 (fail-closed).
#
# Usage:
#   ./scripts/verify-sdk-artifact-attestations.sh [artifact-dir|file ...]
#
# Env:
#   ENABLE_ARTIFACT_ATTESTATIONS  true|1 → private repos produce/expect attestations
#   ATTESTATIONS_REQUIRED         1     → fail if tools missing or verify fails
#   GITHUB_REPOSITORY             owner/repo
#   GITHUB_REPOSITORY_VISIBILITY  public|private|internal
#   GH_TOKEN / GITHUB_TOKEN       auth for gh attestation verify
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

truthy() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

required="$(truthy "${ATTESTATIONS_REQUIRED:-0}" && echo 1 || echo 0)"
enabled_private="$(truthy "${ENABLE_ARTIFACT_ATTESTATIONS:-0}" && echo 1 || echo 0)"

soft_skip() {
  local reason="$1"
  if [[ "$required" == "1" ]]; then
    echo "error: $reason (ATTESTATIONS_REQUIRED=1)" >&2
    exit 1
  fi
  echo "soft-skip: $reason"
  exit 0
}

# Collect subjects: dirs expand to known attestable patterns; files are used as-is.
subjects=()
if [[ $# -eq 0 ]]; then
  set -- "$root/dist/sdk-artifacts"
fi

for arg in "$@"; do
  if [[ -d "$arg" ]]; then
    while IFS= read -r -d '' f; do
      subjects+=("$f")
    done < <(
      find "$arg" -maxdepth 1 -type f \( \
        -name 'SHA256SUMS' -o \
        -name '*.whl' -o \
        -name '*.gem' -o \
        -name '*.tar.gz' -o \
        -name '*package-info.txt' -o \
        -name 'sdk-release-index.json' \
      \) -print0 | sort -z
    )
  elif [[ -f "$arg" ]]; then
    subjects+=("$arg")
  else
    soft_skip "path not found: $arg"
  fi
done

if [[ ${#subjects[@]} -eq 0 ]]; then
  soft_skip "no attestable SDK artifacts"
fi

if ! command -v gh >/dev/null 2>&1; then
  soft_skip "gh CLI not on PATH"
fi

if ! gh attestation --help >/dev/null 2>&1; then
  soft_skip "gh attestation subcommand unavailable"
fi

repo="${GITHUB_REPOSITORY:-}"
if [[ -z "$repo" ]]; then
  if ! repo="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)"; then
    soft_skip "GITHUB_REPOSITORY unset and gh repo view failed"
  fi
fi

visibility="${GITHUB_REPOSITORY_VISIBILITY:-}"
if [[ -z "$visibility" ]]; then
  visibility="$(gh api "repos/$repo" --jq .visibility 2>/dev/null || echo unknown)"
fi

if [[ "$visibility" == "private" || "$visibility" == "internal" ]]; then
  if [[ "$enabled_private" != "1" ]]; then
    soft_skip \
      "private/internal repo $repo without ENABLE_ARTIFACT_ATTESTATIONS (GHEC opt-in)"
  fi
fi

echo "Verifying GitHub artifact attestations for ${#subjects[@]} subject(s) in $repo"
failed=0
for subject in "${subjects[@]}"; do
  base="$(basename "$subject")"
  echo "gh attestation verify --repo $repo -- $base"
  if ! gh attestation verify "$subject" --repo "$repo"; then
    echo "error: attestation verify failed for $base" >&2
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "SDK artifact attestation verification FAILED" >&2
  exit 1
fi

echo "SDK artifact attestation verification OK (${#subjects[@]} subjects, repo=$repo)"
