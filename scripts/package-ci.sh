#!/usr/bin/env bash
# Single entry point for GlifiStore packaging CI.
#
# Resolves the release context from VERSION (never a hard-coded version), expands
# the package matrix for a CI profile, and runs the requested backend adapters.
# Backends without an implementation yet emit honest NOT_RUN / BLOCKED / OPEN_GATE
# evidence; nothing here fabricates a PASS. Workflows call this script instead of
# duplicating packaging logic in YAML.
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: package-ci.sh --profile {pr|main|nightly|release}
                     [--backend NAME]... [--all]
                     [--stage {metadata|build|inspect|install|verify|upgrade|remove|full}]
                     [--package-revision N] [--output-dir DIR] [--release-context FILE]
                     [--candidate DIR]

--candidate is the sealed release-candidate directory; native package lifecycles
admit it through CANDIDATE_SEAL_SHA256 and refuse to build without it.

Exit status is non-zero when a backend reports FAIL or when any tool refuses.
USAGE
  exit 2
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tools="$root/engineering/tools"
profile=""
stage="full"
package_revision=0
output_dir=""
release_context=""
candidate_dir=""
all_backends=0
backends=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) [[ $# -ge 2 ]] || usage; profile="$2"; shift 2 ;;
    --backend) [[ $# -ge 2 ]] || usage; backends+=("$2"); shift 2 ;;
    --all) all_backends=1; shift ;;
    --stage) [[ $# -ge 2 ]] || usage; stage="$2"; shift 2 ;;
    --package-revision) [[ $# -ge 2 ]] || usage; package_revision="$2"; shift 2 ;;
    --output-dir) [[ $# -ge 2 ]] || usage; output_dir="$2"; shift 2 ;;
    --release-context) [[ $# -ge 2 ]] || usage; release_context="$2"; shift 2 ;;
    --candidate) [[ $# -ge 2 ]] || usage; candidate_dir="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "error: unknown argument: $1" >&2; usage ;;
  esac
done

case "$profile" in
  pr|main|nightly|release) ;;
  *) echo "error: --profile is required and must be pr, main, nightly or release" >&2; usage ;;
esac
case "$stage" in
  metadata|build|inspect|install|verify|upgrade|remove|full) ;;
  *) echo "error: unsupported --stage: $stage" >&2; usage ;;
esac
[[ "$package_revision" =~ ^[0-9]+$ ]] || { echo "error: --package-revision must be a non-negative integer" >&2; exit 2; }
if (( all_backends == 1 )) && (( ${#backends[@]} > 0 )); then
  echo "error: --all and --backend are mutually exclusive" >&2
  exit 2
fi
if (( all_backends == 0 )) && (( ${#backends[@]} == 0 )); then
  echo "error: choose --all or at least one --backend" >&2
  exit 2
fi
command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 1; }
if [[ -n "$candidate_dir" ]]; then
  [[ -d "$candidate_dir" ]] || { echo "error: candidate directory is missing: $candidate_dir" >&2; exit 2; }
  candidate_dir="$(cd "$candidate_dir" && pwd -P)"
fi

version="$(<"$root/VERSION")"
commit="$(git -C "$root" rev-parse --short=12 HEAD)"
run_id="${GITHUB_RUN_ID:-local-$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ -z "$output_dir" ]]; then
  output_dir="$root/build/package-ci/$version/$commit/$profile/$run_id"
fi
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd -P)"

if [[ -z "$release_context" ]]; then
  release_context="$output_dir/release-context.json"
  context_arguments=(--root "$root" --package-revision "$package_revision" --output "$release_context" --replace)
  [[ "$profile" == "release" ]] && context_arguments+=(--require-clean)
  python3 "$tools/generate_release_context.py" "${context_arguments[@]}" >/dev/null
fi
[[ -f "$release_context" ]] || { echo "error: release context is missing: $release_context" >&2; exit 1; }

matrix_json="$output_dir/package-matrix.json"
python3 "$tools/generate_package_matrix.py" expand --profile "$profile" \
  --output "$matrix_json" --replace >/dev/null

available=()
while IFS= read -r line; do
  [[ -n "$line" ]] && available+=("$line")
done < <(python3 "$tools/generate_package_matrix.py" backends --profile "$profile")
(( ${#available[@]} > 0 )) || {
  echo "error: the package matrix lists no backend for profile '$profile'" >&2
  exit 1
}

if (( all_backends == 1 )); then
  backends=("${available[@]}")
fi
for backend in "${backends[@]}"; do
  found=0
  for candidate in "${available[@]}"; do
    [[ "$backend" == "$candidate" ]] && found=1
  done
  if (( found == 0 )); then
    echo "error: backend '$backend' does not run in profile '$profile' (available: ${available[*]})" >&2
    exit 2
  fi
done

# Resolves the structural metadata every backend owes, then dispatches to the module
# that owns the backend (scripts/lib/package-backend-*.sh). The module writes its own
# check plan and evidence, so no status is ever decided here.
run_backend() {
  local backend="$1"
  local directory="$output_dir/$backend/$stage"
  mkdir -p "$directory"

  local plan="$directory/check-plan.json"
  local evidence="$directory/$backend-$profile-$stage-package-evidence.json"

  {
    echo "backend=$backend profile=$profile stage=$stage"
    echo "release_context=$release_context"
    echo "package_matrix=$matrix_json"
    echo "product_version=$version package_revision=$package_revision commit=$commit"
    echo "structural metadata resolved without a hard-coded version"
  } >"$directory/structural-metadata.log"

  case "$backend" in
    freebsd|openbsd)
      # The BSD module owns the structural, native-build, package, service and
      # upstream-acceptance rows and the hand-off to the native lifecycle script.
      # shellcheck source=lib/package-backend-bsd.sh
      . "$root/scripts/lib/package-backend-bsd.sh"
      bsd_backend_run "$backend" "$directory" "$plan" "$evidence"
      return
      ;;
    macports|homebrew)
      # The macOS module owns the rendered Portfile/formula, the prefix-isolation
      # rows and the opt-in native lifecycle.
      # shellcheck source=lib/package-backend-macos.sh
      . "$root/scripts/lib/package-backend-macos.sh"
      macos_backend_run "$backend" "$directory"
      return
      ;;
    deb|rpm)
      # The Linux module owns the rendered debian/ or spec, the container or native
      # dispatch, and every package, service and removal row.
      # shellcheck source=lib/package-backend-linux.sh
      . "$root/scripts/lib/package-backend-linux.sh"
      linux_backend_run "$backend" "$directory"
      return
      ;;
    *)
      # Every declared backend owns a module. A backend without one must refuse
      # loudly instead of falling through to a generic, evidence-free success.
      echo "error: no adapter for backend '$backend'" >&2
      return 2
      ;;
  esac
}

status=0
for backend in "${backends[@]}"; do
  run_backend "$backend" || status=1
done

echo "package-ci profile=$profile stage=$stage backends=${backends[*]} output=$output_dir"
exit "$status"
