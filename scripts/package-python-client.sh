#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
python="${PYTHON:-python3}"
sdk="$root/sdk/python"

for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$sdk/tests/fixtures/$fixture"; then
    echo "vendored fixture drift: $fixture" >&2
    echo "copy repository fixtures into sdk/python/tests/fixtures/ before packaging" >&2
    exit 1
  fi
done

expected="$(tr -d '[:space:]' <"$root/VERSION")"
got="$(PYTHONPATH="$sdk/src${PYTHONPATH:+:$PYTHONPATH}" "$python" -c 'import glifistore; print(glifistore.__version__)')"
if [[ "$got" != "$expected" ]]; then
  echo "glifistore.__version__='$got' does not match VERSION='$expected'" >&2
  exit 1
fi

rm -rf "$sdk/dist" "$sdk/build"
find "$sdk" -maxdepth 2 -type d -name '*.egg-info' -exec rm -rf {} +

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-python-pack.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

"$python" -m pip install --disable-pip-version-check -q build twine
rm -rf "$sdk/dist"
mkdir -p "$sdk/dist"
(
  cd "$sdk"
  "$python" -m build --outdir "$sdk/dist"
  "$python" -m twine check "$sdk/dist"/*
)
# Normalize sdist tar metadata (wheels are already bit-stable under SOURCE_DATE_EPOCH).
shopt -s nullglob
for sdist in "$sdk/dist"/*.tar.gz; do
  "$root/scripts/normalize-tar-gz.sh" "$sdist"
done
shopt -u nullglob

shopt -s nullglob
wheels=("$sdk/dist"/glifistore-*.whl)
sdists=("$sdk/dist"/glifistore-*.tar.gz)
shopt -u nullglob
if [[ "${#wheels[@]}" -ne 1 || "${#sdists[@]}" -ne 1 ]]; then
  echo "expected exactly one Python wheel and one sdist" >&2
  exit 1
fi

cp -R "$sdk/tests" "$work/tests"

verify_installed_artifact() {
  local label="$1" artifact="$2" venv="$3"
  "$python" -m venv "$venv"
  "$venv/bin/python" -m pip install --disable-pip-version-check -q --upgrade pip
  "$venv/bin/python" -m pip install --disable-pip-version-check -q --no-deps "$artifact"
  "$venv/bin/python" - "$label" <<'PY'
import glifistore
import sys
from importlib.metadata import metadata, version

label = sys.argv[1]
assert glifistore.__version__ == version("glifistore")
meta = metadata("glifistore")
license_files = {p for p in (meta.get_all("License-File") or [])}
# setuptools may list basenames or package-relative paths
names = {p.split("/")[-1] for p in license_files} | license_files
for required in ("LICENSE", "NOTICE"):
    if required not in names and not any(required in p for p in license_files):
        raise SystemExit(f"missing license file in installed {label} metadata: {required}")
print(f"installed {label} glifistore {glifistore.__version__} (LICENSE/NOTICE present)")
PY
  (
    cd "$work"
    PYTHONPATH= PYTHONWARNINGS="error::ResourceWarning${PYTHONWARNINGS:+,$PYTHONWARNINGS}" \
      "$venv/bin/python" -m unittest discover -s "$work/tests" -v
  )
  echo "Installed Python $label conformance OK ($artifact)"
}

verify_installed_artifact wheel "${wheels[0]}" "$work/wheel-venv"
verify_installed_artifact sdist "${sdists[0]}" "$work/sdist-venv"
echo "Python packaging verification OK ($sdk/dist)"
