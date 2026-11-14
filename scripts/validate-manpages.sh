#!/usr/bin/env bash
# Validate GlifiStore mdoc man pages for Linux/macOS/FreeBSD/OpenBSD.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
man_root="${1:-}"

if [[ -z "${man_root}" ]]; then
  # Prefer configured build output when present; else lint .in sources with a
  # temporary version/date substitution.
  if [[ -d "${root}/build/macos-ci/man" ]]; then
    man_root="${root}/build/macos-ci/man"
  elif [[ -d "${root}/build/man" ]]; then
    man_root="${root}/build/man"
  else
    man_root=""
  fi
fi

version="$(tr -d '[:space:]' <"${root}/VERSION")"
date_stamp="${GLIFISTORE_MAN_DATE:-August 1, 2026}"

expected=(
  "man1/glifistore_demo.1"
  "man1/glifistore_inspect_segment.1"
  "man1/glifistore_verify_store.1"
  "man1/glifistore_backup_store.1"
  "man1/glifistore_migrate_store.1"
  "man1/glifistore_repair_store.1"
  "man1/glifistore_rebuild_index.1"
  "man7/glifistore.7"
  "man8/glifistored.8"
)

tmpdir=""
cleanup() {
  if [[ -n "${tmpdir}" && -d "${tmpdir}" ]]; then
    rm -rf "${tmpdir}"
  fi
}
trap cleanup EXIT

if [[ -z "${man_root}" ]]; then
  tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-man.XXXXXX")"
  man_root="${tmpdir}"
  for rel in "${expected[@]}"; do
    src="${root}/man/${rel}.in"
    dst="${man_root}/${rel}"
    mkdir -p "$(dirname "${dst}")"
    sed -e "s/@PROJECT_VERSION@/${version}/g" -e "s/@MAN_DATE@/${date_stamp}/g" \
      "${src}" >"${dst}"
  done
fi

missing=0
for rel in "${expected[@]}"; do
  if [[ ! -f "${man_root}/${rel}" && ! -f "${man_root}/${rel}.gz" ]]; then
    echo "error: missing man page ${rel}" >&2
    missing=1
  fi
done
if [[ "${missing}" -ne 0 ]]; then
  exit 1
fi

# Every Runtime CLI binary must have a dedicated page (overview is extra).
runtime_bins=(
  glifistored
  glifistore_demo
  glifistore_inspect_segment
  glifistore_verify_store
  glifistore_backup_store
  glifistore_migrate_store
  glifistore_repair_store
  glifistore_rebuild_index
)
for bin in "${runtime_bins[@]}"; do
  if ! ls "${man_root}/man"*"/""${bin}".[0-9]* >/dev/null 2>&1; then
    echo "error: no man page for installed binary ${bin}" >&2
    exit 1
  fi
done

lint_one() {
  local file="$1"
  local view="${file}"
  local tmp_plain=""
  if [[ "${file}" == *.gz ]]; then
    tmp_plain="$(mktemp "${TMPDIR:-/tmp}/glifistore-manpage.XXXXXX")"
    gzip -dc "${file}" >"${tmp_plain}"
    view="${tmp_plain}"
  fi
  if command -v mandoc >/dev/null 2>&1; then
    # -W error treats style warnings as failures for release hygiene.
    if ! mandoc -T lint -W error "${view}" 2>&1; then
      echo "error: mandoc lint failed for ${file}" >&2
      [[ -n "${tmp_plain}" ]] && rm -f "${tmp_plain}"
      return 1
    fi
    # Ensure the page renders (catman path).
    mandoc -T ascii "${view}" >/dev/null
  elif command -v groff >/dev/null 2>&1; then
    groff -mandoc -Tutf8 -z "${view}" 2>&1
  else
    echo "error: need mandoc or groff to validate man pages" >&2
    [[ -n "${tmp_plain}" ]] && rm -f "${tmp_plain}"
    return 1
  fi
  [[ -n "${tmp_plain}" ]] && rm -f "${tmp_plain}"
  return 0
}

failed=0
while IFS= read -r -d '' file; do
  if ! lint_one "${file}"; then
    failed=1
  fi
done < <(find "${man_root}" \( -name '*.[1-9]' -o -name '*.[1-9].gz' \) -print0 | sort -z)

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "Man page validation OK (${#expected[@]} pages under ${man_root})."
