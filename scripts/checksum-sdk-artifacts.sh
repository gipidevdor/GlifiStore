#!/usr/bin/env bash
# Write SHA-256 checksums, a release index, and optional SBOMs for packaged SDK artifacts.
# Set SYFT_REQUIRED=1 to fail closed when syft is missing (CI supply-chain gate).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${1:-$root/dist/sdk-artifacts}"
rm -rf "$outdir"
mkdir -p "$outdir"

if command -v shasum >/dev/null 2>&1; then
  digest() { shasum -a 256 "$1"; }
elif command -v sha256sum >/dev/null 2>&1; then
  digest() { sha256sum "$1"; }
else
  echo "need shasum or sha256sum" >&2
  exit 1
fi

manifest="$outdir/SHA256SUMS"
: >"$manifest"

add() {
  local path="$1"
  local dest_name="${2:-$(basename "$path")}"
  [[ -f "$path" ]] || return 0
  cp "$path" "$outdir/$dest_name"
  (
    cd "$outdir"
    digest "$dest_name"
  ) >>"$manifest"
  echo "checksummed $dest_name"
}

shopt -s nullglob
# Distinct names: macOS default FS is case-insensitive (glifistore vs GlifiStore).
for f in "$root/sdk/python/dist"/glifistore-*.whl; do
  add "$f"
done
for f in "$root/sdk/python/dist"/glifistore-*.tar.gz; do
  add "$f" "python-$(basename "$f")"
done
for f in "$root/sdk/perl/dist"/GlifiStore-*.tar.gz; do
  add "$f" "perl-$(basename "$f")"
done
for f in "$root/sdk/ruby/dist"/glifistore-*.gem; do
  add "$f"
done
for f in "$root/sdk/erlang/dist"/glifistore-erlang-*.tar.gz; do
  add "$f"
done
for f in "$root/sdk/go/dist"/glifistore-go-*.tar.gz; do
  add "$f"
done
add "$root/sdk/go/dist/package-info.txt" "go-package-info.txt"
add "$root/sdk/erlang/dist/package-info.txt" "erlang-package-info.txt"
add "$root/sdk/cpp-dist/package-info.txt" "cpp-package-info.txt"
shopt -u nullglob

if [[ -s "$manifest" ]]; then
  sort -u "$manifest" -o "$manifest"
fi

if [[ "${SDK_ARTIFACT_INDEX_REQUIRE_COMPLETE:-0}" == "1" ]]; then
  python3 "$root/engineering/tools/write_sdk_release_index.py" "$outdir" --require-complete
else
  python3 "$root/engineering/tools/write_sdk_release_index.py" "$outdir"
fi

syft_available=0
if command -v syft >/dev/null 2>&1; then
  syft_available=1
fi

if [[ "${SYFT_REQUIRED:-0}" == "1" && "$syft_available" != "1" ]]; then
  echo "SYFT_REQUIRED=1 but syft is not on PATH" >&2
  exit 1
fi

if [[ "$syft_available" == "1" ]]; then
  sbom_count=0
  for f in "$outdir"/*; do
    [[ -f "$f" && "$(basename "$f")" != "SHA256SUMS" && "$f" != *.spdx.json && "$f" != SBOM.README ]] || continue
    base="$(basename "$f")"
    syft "file:$f" -o "spdx-json=$outdir/${base}.spdx.json"
    sbom_count=$((sbom_count + 1))
  done
  if [[ "$sbom_count" -eq 0 && "${SYFT_REQUIRED:-0}" == "1" ]]; then
    echo "SYFT_REQUIRED=1 but no packaged artifacts to SBOM under $outdir" >&2
    exit 1
  fi
  echo "SBOM: syft SPDX JSON written for $sbom_count artifact(s)"
else
  cat >"$outdir/SBOM.README" <<'EOF'
No SBOM generator found on PATH.

For release perfection, generate SPDX/CycloneDX with one of:
  - syft file:./artifact -o spdx-json=artifact.spdx.json
  - cyclonedx-py / cyclonedx-gomod / etc.

CI sets SYFT_REQUIRED=1 so this README path is not accepted on the supply-chain gate.
Attach SBOM + SHA256SUMS to the GitHub Release. Sign with Sigstore (cosign)
or project GPG once keys/OIDC are configured.
EOF
fi

echo "Artifact checksums written to $manifest"
