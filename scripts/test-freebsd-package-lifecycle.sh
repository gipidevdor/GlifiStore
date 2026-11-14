#!/usr/bin/env bash
# Build the tagged FreeBSD port and prove the installed package/service lifecycle.
set -euo pipefail

usage() {
  echo "usage: $0 <candidate-directory> <output-directory>" >&2
  exit 2
}

[[ $# -eq 2 ]] || usage
[[ "$(uname -s)" == "FreeBSD" ]] || {
  echo "error: native FreeBSD is required" >&2
  exit 1
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
candidate="$(cd "$1" && pwd -P)"
mkdir -p "$2"
output="$(cd "$2" && pwd -P)"
version="$(<"$root/VERSION")"
ports_root="${PORTSDIR:-/usr/ports}"

[[ -n "${CANDIDATE_SEAL_SHA256:-}" ]] || {
  echo "error: CANDIDATE_SEAL_SHA256 is required" >&2
  exit 1
}
command -v python3 >/dev/null 2>&1 || { echo "error: python3 required" >&2; exit 1; }
command -v pkg >/dev/null 2>&1 || { echo "error: pkg required" >&2; exit 1; }
[[ -f "$ports_root/Mk/bsd.port.mk" ]] || {
  echo "error: complete native ports tree required at $ports_root" >&2
  exit 1
}

python3 - "$candidate/candidate-seal.json" "$CANDIDATE_SEAL_SHA256" <<'PY'
import hashlib
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
actual = hashlib.sha256(path.read_bytes()).hexdigest()
if actual != sys.argv[2]:
    raise SystemExit(f"candidate seal digest mismatch: {actual}")
PY
python3 "$root/engineering/tools/release_bundle.py" verify-seal \
  --directory "$candidate" --seal candidate-seal.json
python3 "$root/engineering/tools/validate_bsd_packaging.py" --root "$root" --release

grep -Eq '^glifistore:' "$ports_root/UIDs" || {
  echo "error: glifistore is not registered in the native FreeBSD ports UIDs authority" >&2
  exit 1
}
grep -Eq '^glifistore:' "$ports_root/GIDs" || {
  echo "error: glifistore is not registered in the native FreeBSD ports GIDs authority" >&2
  exit 1
}

mapfile -t source_archives < <(
  find "$candidate" -maxdepth 1 -type f -name "GlifiStore-$version.tar.xz" -print
)
[[ ${#source_archives[@]} -eq 1 ]] || {
  echo "error: expected exactly one sealed source archive" >&2
  exit 1
}

work="$(mktemp -d /tmp/glifistore-freebsd-package.XXXXXX)"
cleanup() {
  service glifistored stop >/dev/null 2>&1 || true
  if pkg info -e glifistore >/dev/null 2>&1; then
    pkg delete -y glifistore >/dev/null 2>&1 || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

if pkg info -e glifistore >/dev/null 2>&1; then
  echo "error: lifecycle proof requires a clean host without glifistore installed" >&2
  exit 1
fi

mkdir -p "$work/port" "$work/distfiles" "$work/packages"
cp -R "$root/packaging/freebsd/." "$work/port/"
cp "${source_archives[0]}" "$work/distfiles/GlifiStore-$version.tar.xz"

package_build_log="$output/freebsd-package-build.log"
{
  make -C "$work/port" DISTDIR="$work/distfiles" makesum
  make -C "$work/port" DISTDIR="$work/distfiles" checksum
  make -C "$work/port" DISTDIR="$work/distfiles" clean stage
  make -C "$work/port" DISTDIR="$work/distfiles" check-plist
  make -C "$work/port" DISTDIR="$work/distfiles" PACKAGES="$work/packages" package
  echo "FREEBSD-PACKAGE package-build PASSED"
} 2>&1 | tee "$package_build_log"
test -s "$work/port/distinfo"
cp "$work/port/distinfo" "$output/freebsd-distinfo"

mapfile -t built_packages < <(find "$work/packages" -type f -name '*.pkg' -print)
[[ ${#built_packages[@]} -eq 1 ]] || {
  echo "error: expected exactly one native FreeBSD package" >&2
  exit 1
}
freebsd_version="$(freebsd-version -u | cut -d- -f1)"
architecture="$(uname -p)"
package="$output/glifistore-$version-freebsd$freebsd_version-$architecture.pkg"
cp "${built_packages[0]}" "$package"

# shellcheck source=scripts/lib/bsd-package-upgrade.sh
source "$root/scripts/lib/bsd-package-upgrade.sh"
upgrade_exercised=0
previous_version="$(bsd_n1_previous_version)" || exit 1
if [[ -n "$previous_version" ]]; then
  mapfile -t n1_packages < <(bsd_n1_select_packages freebsd "$previous_version") || exit 1
  (( ${#n1_packages[@]} >= 1 )) || {
    echo "error: no sealed FreeBSD N-1 packages for $previous_version" >&2
    exit 1
  }
  upgrade_seed_key_hex="6273642d7061636b6167652d75706772616465"
  upgrade_seed_value_hex="64757261626c652d6163726f73732d6e312d75706772616465"
  client=(python3 "$root/scripts/sdk_interop_py.py" --host 127.0.0.1 --port 7379)
  {
    echo "previous_version=$previous_version"
    echo "GLIFISTORE_N1_PACKAGE_DIR=$GLIFISTORE_N1_PACKAGE_DIR"
    printf 'n1_packages=%s\n' "${n1_packages[*]}"
    echo "n_package=$package"
    echo "installing sealed N-1 before N"
    for n1_package in "${n1_packages[@]}"; do
      pkg add -y "$n1_package"
    done
    pkg info -e "glifistore-$previous_version"
    id glifistore
    sysrc glifistored_enable=YES
    service glifistored start
    for _ in $(jot 50 1); do
      sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379' && break
      sleep 1
    done
    sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379'
    PYTHONPATH="$root/sdk/python/src" "${client[@]}" put \
      --key-hex "$upgrade_seed_key_hex" --value-hex "$upgrade_seed_value_hex"
    service glifistored stop
    echo "seeded durable value under N-1"
    echo "upgrading to N package: $package"
    pkg add -y "$package"
    pkg info -e "glifistore-$version"
    service glifistored start
    for _ in $(jot 50 1); do
      sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379' && break
      sleep 1
    done
    sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379'
    got="$(PYTHONPATH="$root/sdk/python/src" "${client[@]}" get \
      --key-hex "$upgrade_seed_key_hex")"
    [[ "$got" == "$upgrade_seed_value_hex" ]]
    service glifistored stop
    echo "seeded value recovered byte-exact after upgrade to N"
    echo "FREEBSD-PACKAGE package-upgrade PASSED"
  } 2>&1 | tee "$output/freebsd-package-upgrade.log"
  {
    echo "upgrade path installed N from sealed N-1 ($previous_version)"
    pkg info -e "glifistore-$version"
    id glifistore
    echo "FREEBSD-PACKAGE package-install PASSED"
  } 2>&1 | tee "$output/freebsd-package-install.log"
  upgrade_exercised=1
fi

if [[ "$upgrade_exercised" -eq 0 ]]; then
{
  pkg add -y "$package"
  pkg info -e "glifistore-$version"
  id glifistore
  echo "FREEBSD-PACKAGE package-install PASSED"
} 2>&1 | tee "$output/freebsd-package-install.log"
fi

{
  pkg info -l glifistore
  pkg check -s glifistore
  test -x /usr/local/bin/glifistored
  test -f /usr/local/etc/glifistored.conf
  test -f /usr/local/lib/libglifistore.so.1
  python3 "$root/engineering/tools/check_abi_symbols.py" \
    --library /usr/local/lib/libglifistore.so.1 \
    --allowlist "$root/abi/symbols-v1.txt"
  echo "FREEBSD-PACKAGE file-inventory PASSED"
} 2>&1 | tee "$output/freebsd-file-inventory.log"

{
  chmod +x "$root/scripts/lib/package-external-consumer.sh"
  "$root/scripts/lib/package-external-consumer.sh" \
    "$root" /usr/local "$work"
  echo "FREEBSD-PACKAGE external-consumer PASSED"
} 2>&1 | tee "$output/freebsd-external-consumer.log"

{
  sysrc glifistored_enable=YES
  service glifistored start
  for _ in $(jot 50 1); do
    sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379' && break
    sleep 1
  done
  sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379'
  pgrep -U glifistore -f '/usr/local/bin/glifistored' >/dev/null
  echo "FREEBSD-PACKAGE service-start PASSED"
} 2>&1 | tee "$output/freebsd-service-start.log"

key_hex="667265656273642d7061636b616765"
value_hex="6e61746976652d6c6966656379636c65"
recovery_key_hex="667265656273642d7265636f76657279"
recovery_value_hex="64757261626c652d72657374617274"
client=(python3 "$root/scripts/sdk_interop_py.py" --host 127.0.0.1 --port 7379)
{
  PYTHONPATH="$root/sdk/python/src" "${client[@]}" put \
    --key-hex "$key_hex" --value-hex "$value_hex"
  got="$(PYTHONPATH="$root/sdk/python/src" "${client[@]}" get --key-hex "$key_hex")"
  [[ "$got" == "$value_hex" ]]
  PYTHONPATH="$root/sdk/python/src" "${client[@]}" erase --key-hex "$key_hex"
  PYTHONPATH="$root/sdk/python/src" "${client[@]}" expect-not-found --key-hex "$key_hex"
  echo "FREEBSD-PACKAGE put-get-erase PASSED"
} 2>&1 | tee "$output/freebsd-put-get-erase.log"

{
  PYTHONPATH="$root/sdk/python/src" "${client[@]}" put \
    --key-hex "$recovery_key_hex" --value-hex "$recovery_value_hex"
  service glifistored stop
  ! pgrep -U glifistore -f '/usr/local/bin/glifistored' >/dev/null
  /usr/local/bin/glifistore_verify_store -- /var/db/glifistore
  echo "FREEBSD-PACKAGE graceful-shutdown PASSED"
} 2>&1 | tee "$output/freebsd-graceful-shutdown.log"

{
  service glifistored start
  for _ in $(jot 50 1); do
    sockstat -4 -l | grep -Eq '127\.0\.0\.1:7379' && break
    sleep 1
  done
  got="$(PYTHONPATH="$root/sdk/python/src" "${client[@]}" get \
    --key-hex "$recovery_key_hex")"
  [[ "$got" == "$recovery_value_hex" ]]
  service glifistored stop
  echo "FREEBSD-PACKAGE restart-recovery PASSED"
} 2>&1 | tee "$output/freebsd-restart-recovery.log"

config_marker="# retained-config-${GITHUB_RUN_ID:-local}"
{
  echo "$config_marker" >>/usr/local/etc/glifistored.conf
  pkg add -f -y "$package"
  grep -Fqx "$config_marker" /usr/local/etc/glifistored.conf
  echo "FREEBSD-PACKAGE config-preservation PASSED"
} 2>&1 | tee "$output/freebsd-config-preservation.log"

{
  pkg delete -y glifistore
  ! pkg info -e glifistore >/dev/null 2>&1
  test ! -e /usr/local/bin/glifistored
  grep -Fqx "$config_marker" /usr/local/etc/glifistored.conf
  test -d /var/db/glifistore
  echo "FREEBSD-PACKAGE uninstall PASSED"
} 2>&1 | tee "$output/freebsd-uninstall.log"

python3 - "$work/checks.json" "$upgrade_exercised" <<'PY'
import json
import sys

plan = [
  {"id":"package-build","command":"native FreeBSD ports checksum, stage, check-plist and package against the sealed source archive","evidence_ref":"freebsd-package-build.log"},
  {"id":"package-install","command":"pkg add the native package and prove the dedicated service account","evidence_ref":"freebsd-package-install.log"},
  {"id":"file-inventory","command":"pkg inventory/checksum closure plus exact C ABI symbol allowlist","evidence_ref":"freebsd-file-inventory.log"},
  {"id":"external-consumer","command":"cmake/ctest packaging/common/consumer against /usr/local outside the checkout with isolation refused","evidence_ref":"freebsd-external-consumer.log"},
  {"id":"service-start","command":"enable and start the rc.subr service as glifistore on loopback","evidence_ref":"freebsd-service-start.log"},
  {"id":"put-get-erase","command":"protocol-v2 PUT, exact GET, ERASE and NOT_FOUND through the packaged service","evidence_ref":"freebsd-put-get-erase.log"},
  {"id":"graceful-shutdown","command":"persist a recovery key, stop through rc.subr, prove process exit and verify the Store","evidence_ref":"freebsd-graceful-shutdown.log"},
  {"id":"restart-recovery","command":"restart the packaged service and recover the exact durable value","evidence_ref":"freebsd-restart-recovery.log"},
  {"id":"uninstall","command":"pkg delete removes package-owned executables while preserving non-empty state","evidence_ref":"freebsd-uninstall.log"},
  {"id":"config-preservation","command":"force reinstall preserves an operator-modified @sample configuration","evidence_ref":"freebsd-config-preservation.log"},
]
if sys.argv[2] == "1":
  plan.insert(
    2,
    {
      "id": "package-upgrade",
      "command": "install sealed N-1, seed durable data, upgrade to N, verify byte-exact continuity",
      "evidence_ref": "freebsd-package-upgrade.log",
    },
  )
json.dump(plan, open(sys.argv[1], "w", encoding="utf-8"), indent=2)
print()
PY

env RUNNER_OS=FreeBSD RUNNER_ARCH="$architecture" \
  python3 "$root/engineering/tools/release_evidence.py" create \
  --root "$root" --type freebsd_package --subject "$package" \
  --output "$output/freebsd-package-evidence.json" --check-plan "$work/checks.json" \
  --limitation 'native FreeBSD VM package/service lifecycle; not UFS/ZFS power-loss certification or an official ports-tree acceptance claim' \
  --require-ci

echo "FreeBSD native package lifecycle PASSED"
