#!/usr/bin/env bash
# Copyright / license hygiene: notice files, exact SDK LICENSE sync, REUSE, Go/Python dep scan.
# Fail closed on exclusive GPL/AGPL/SSPL runtime deps. See docs/legal/licensing.md.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
failed=0

echo "== Required notice / license files =="
for path in \
  LICENSE NOTICE THIRD_PARTY_NOTICES.md docs/legal/licensing.md \
  docs/legal/tls-redistribution.md REUSE.toml LICENSES/BSD-3-Clause.txt; do
  if [[ ! -f "$path" ]]; then
    echo "ERROR: missing $path" >&2
    failed=1
  fi
done
for path in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
  if ! grep -q 'Giacomo Picchiarelli' "$path"; then
    echo "ERROR: $path must name the copyright holder" >&2
    failed=1
  fi
done
if ! grep -q 'BSD-3-Clause\|BSD 3-Clause' LICENSE; then
  echo "ERROR: LICENSE must be BSD-3-Clause" >&2
  failed=1
fi
if ! grep -q 'THIRD_PARTY_NOTICES.md' NOTICE; then
  echo "ERROR: NOTICE must point at THIRD_PARTY_NOTICES.md" >&2
  failed=1
fi
if ! grep -q 'OpenSSL\|LibreSSL\|SipHash\|SwissTable\|CRC32C' THIRD_PARTY_NOTICES.md; then
  echo "ERROR: THIRD_PARTY_NOTICES.md incomplete (expected OpenSSL/LibreSSL/SipHash/SwissTable/CRC32C)" >&2
  failed=1
fi
if ! grep -q 'tls-redistribution.md' THIRD_PARTY_NOTICES.md; then
  echo "ERROR: THIRD_PARTY_NOTICES.md must link docs/legal/tls-redistribution.md" >&2
  failed=1
fi
if ! grep -q 'THIRD_PARTY_NOTICES.md' CMakeLists.txt; then
  echo "ERROR: CMake must install THIRD_PARTY_NOTICES.md" >&2
  failed=1
fi
echo "Notice files OK"

echo "== SDK LICENSE / NOTICE sync =="
for sdk in python go perl ruby erlang; do
  sdk_license="sdk/$sdk/LICENSE"
  sdk_notice="sdk/$sdk/NOTICE"
  if [[ ! -f "$sdk_license" ]]; then
    echo "ERROR: missing $sdk_license" >&2
    failed=1
    continue
  fi
  if ! cmp -s LICENSE "$sdk_license"; then
    echo "ERROR: $sdk_license must be byte-identical to root LICENSE" >&2
    failed=1
  fi
  if [[ ! -f "$sdk_notice" ]]; then
    echo "ERROR: missing $sdk_notice" >&2
    failed=1
  elif ! grep -q 'Giacomo Picchiarelli' "$sdk_notice"; then
    echo "ERROR: $sdk_notice must name the copyright holder" >&2
    failed=1
  fi
done
echo "SDK LICENSE / NOTICE sync OK"

echo "== REUSE lint =="
python3 -m venv "$tmpdir/reuse-venv"
# shellcheck disable=SC1091
source "$tmpdir/reuse-venv/bin/activate"
python -m pip install --disable-pip-version-check --quiet 'reuse[charset-normalizer]>=5' >/dev/null
if ! reuse lint; then
  echo "ERROR: reuse lint failed" >&2
  failed=1
else
  echo "REUSE lint OK"
fi
deactivate

echo "== Go licenses (sdk/go) =="
if ! command -v go >/dev/null 2>&1; then
  echo "go toolchain required" >&2
  exit 1
fi
# sdk/go is currently stdlib-only. go-licenses@v1.6.0 exits non-zero on such modules
# with fatal stdlib noise; treat "no third-party requires" as an explicit pass and only
# invoke the scanner when real module deps exist (fail-closed).
go_module_count="$(go -C "$root/sdk/go" list -m all | wc -l | tr -d '[:space:]')"
if [[ "$go_module_count" -le 1 ]]; then
  echo "Go license scan OK (stdlib-only module; no third-party requires)"
else
  GOBIN="$tmpdir" go install github.com/google/go-licenses@v1.6.0
  if ! (
    cd "$root/sdk/go"
    "$tmpdir/go-licenses" report ./... >"$tmpdir/go-licenses.txt"
  ); then
    echo "ERROR: go-licenses report failed" >&2
    failed=1
  elif [[ -s "$tmpdir/go-licenses.txt" ]] && grep -Ei 'GPL|AGPL|SSPL|Commons Clause' "$tmpdir/go-licenses.txt"; then
    echo "ERROR: disallowed copyleft/commercial-clause license in Go deps" >&2
    failed=1
  else
    echo "Go license scan OK ($(wc -l <"$tmpdir/go-licenses.txt" | tr -d ' ') rows)"
  fi
fi

echo "== Python licenses (sdk/python) =="
venv="$tmpdir/pyvenv"
python3 -m venv "$venv"
# shellcheck disable=SC1091
source "$venv/bin/activate"
python -m pip install --disable-pip-version-check --quiet 'pip-licenses>=5' >/dev/null
python -m pip install --disable-pip-version-check --quiet -e "$root/sdk/python" >/dev/null
pip-licenses --format=plain --with-urls >"$tmpdir/pip-licenses.txt"
deactivate

if [[ -s "$tmpdir/pip-licenses.txt" ]]; then
  if ! python3 - "$tmpdir/pip-licenses.txt" <<'PY'
import re, sys
path = sys.argv[1]
bad = []
for line in open(path, encoding="utf-8"):
    if not line.strip() or line.lower().startswith("name") or set(line.strip()) <= {"-", "="}:
        continue
    parts = re.split(r"\s{2,}", line.strip())
    if len(parts) < 3:
        continue
    name, license_name = parts[0], parts[2]
    if name.lower() in {"glifistore", "pip", "setuptools", "wheel", "pip-licenses"}:
        continue
    has_copyleft = re.search(r"\b(AGPL|GPL|SSPL|Commons Clause)\b", license_name, re.I)
    has_permissive = re.search(
        r"\b(Apache|BSD|MIT|ISC|MPL|Unlicense|0BSD|CC0|PSF|Python|Zlib|Public Domain|BlueOak)\b",
        license_name,
        re.I,
    )
    if has_copyleft and not has_permissive:
        bad.append(f"{name}: {license_name}")
if bad:
    print("ERROR: exclusive copyleft licenses:\n" + "\n".join(bad), file=sys.stderr)
    sys.exit(1)
PY
  then
    failed=1
  else
    echo "Python license scan OK"
    head -20 "$tmpdir/pip-licenses.txt" || true
  fi
else
  echo "ERROR: empty Python license report" >&2
  failed=1
fi

if [[ "$failed" -ne 0 ]]; then
  echo "Copyright / license check FAILED" >&2
  exit 1
fi
echo "Copyright / license check OK"
