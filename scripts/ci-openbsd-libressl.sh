#!/usr/bin/env bash
# OpenBSD / LibreSSL CI gate (security roadmap Phase 2.6 + Phase 6.5, ADR 0020).
#
# Builds glifistored with system LibreSSL, runs the unit/integration suite,
# and smokes TLS PUT→GET with the official Go client (including pledge/unveil
# apply after Server::create).
#
# Intended for:
#   - GitHub Actions via vmactions/openbsd-vm
#   - Native OpenBSD developer hosts
#
# Environment:
#   GLIFISTORE_OPENBSD_PRESET   cmake preset (default: unix-release)
#   GLIFISTORE_TEST_SHARD_COUNT number of deterministic test shards (optional)
#   GLIFISTORE_TEST_SHARD_INDEX zero-based shard index (set with shard count)
#   GLIFISTORE_SKIP_GO_SMOKE    set to 1 to skip Go TLS smoke
#   CC / CXX                     optional compiler overrides
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${GLIFISTORE_OPENBSD_PRESET:-unix-release}"
build_dir="$root/build/${preset}"

if [[ "$(uname -s)" != "OpenBSD" ]]; then
  echo "error: this gate must run on OpenBSD (got $(uname -s))" >&2
  echo "hint: use .github/workflows/openbsd-libressl.yml or a native OpenBSD host" >&2
  exit 1
fi

echo "== OpenBSD / LibreSSL CI gate =="
echo "os=$(uname -a)"
echo "cwd=$root"
echo "preset=$preset"

if [[ ! -f /usr/include/openssl/ssl.h ]]; then
  echo "error: system LibreSSL headers missing (/usr/include/openssl/ssl.h)" >&2
  exit 1
fi
# OpenBSD ships versioned shared objects (libssl.so.*); accept those or a static archive.
if ! ls /usr/lib/libssl.so* >/dev/null 2>&1 && [[ ! -e /usr/lib/libssl.a ]]; then
  echo "error: system LibreSSL libssl missing under /usr/lib" >&2
  exit 1
fi

command -v cmake >/dev/null 2>&1 || { echo "error: cmake required (pkg_add cmake)" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "error: ninja required (pkg_add ninja)" >&2; exit 1; }
command -v openssl >/dev/null 2>&1 || { echo "error: openssl CLI required (LibreSSL base)" >&2; exit 1; }

echo "== configure (GLIFISTORE_ENABLE_TLS=ON) =="
rm -rf "$build_dir"
cmake --preset "$preset" -DGLIFISTORE_ENABLE_TLS=ON | tee /tmp/glifistore-cmake-tls.log
if ! grep -q 'GlifiStore TLS: enabled (LibreSSL)' /tmp/glifistore-cmake-tls.log; then
  echo "error: expected CMake to enable TLS with LibreSSL backend" >&2
  exit 1
fi

echo "== build =="
cmake --build --preset "$preset" --target \
  glifistored glifistore_tests glifistore_security_audit_sink_tests glifistore_interop_client \
  glifistore_abi glifistore_c_abi_tests glifistore_c_abi_durable_tests

daemon="$build_dir/glifistored"
tests="$build_dir/glifistore_tests"
if [[ ! -x "$daemon" ]]; then
  echo "error: missing $daemon" >&2
  exit 1
fi
if ! "$daemon" --help 2>&1 | grep -q -- '--tls-cert'; then
  echo "error: glifistored was built without TLS CLI flags" >&2
  exit 1
fi

echo "== ctest (built targets only; verbose, 15-minute per-target timeout) =="
# This gate builds glifistored + glifistore_tests (+ Go interop smoke). Offline CLI
# tools, crash harnesses, and benchmarks are not built here; match only binaries that
# exist so missing executables are not counted as failures. Verbose mode streams each
# test-harness [RUN] marker, preserving the exact stalled case if the monolith times out.
ctest --preset "$preset" --verbose --timeout 900 -R '^(glifistore_tests|glifistore_audit_sink_|glifistore_cli_daemon_)'

echo "== installed C ABI consumer =="
install_root="$(mktemp -d /tmp/glifistore-openbsd-install.XXXXXX)"
consumer_build="$(mktemp -d /tmp/glifistore-openbsd-consumer.XXXXXX)"
cleanup_install() {
  rm -rf "$install_root" "$consumer_build"
}
trap cleanup_install EXIT
cmake --install "$build_dir" --prefix "$install_root" --component AbiRuntime
cmake --install "$build_dir" --prefix "$install_root" --component Development
test -f "$install_root/include/glifistore/abi/glifistore.h"
find "$install_root/lib" -maxdepth 1 -name 'libglifistore.so.[0-9]*' -type f | grep -q .

cmake -S tests/consumer -B "$consumer_build" -G Ninja \
  -DCMAKE_PREFIX_PATH="$install_root"
cmake --build "$consumer_build" --target glifistore_abi_consumer_smoke
LD_LIBRARY_PATH="$install_root/lib" "$consumer_build/glifistore_abi_consumer_smoke"

PKG_CONFIG_PATH="$install_root/lib/pkgconfig" \
  pkg-config --cflags --libs glifistore-abi >"$consumer_build/abi.flags"
# shellcheck disable=SC2046
cc -std=c11 tests/consumer/abi.c \
  $(cat "$consumer_build/abi.flags") -o "$consumer_build/pkgconfig-abi-consumer"
LD_LIBRARY_PATH="$install_root/lib" "$consumer_build/pkgconfig-abi-consumer"

if [[ "${GLIFISTORE_SKIP_GO_SMOKE:-0}" == "1" ]]; then
  echo "== skip Go TLS smoke (GLIFISTORE_SKIP_GO_SMOKE=1) =="
  echo "OpenBSD / LibreSSL CI gate OK (tests only)"
  exit 0
fi

# OpenBSD packages often lag the SDK language floor and pin GOTOOLCHAIN=local.
# Bootstrap the same Go pin used by Linux packaging when the ports go is too old.
ensure_go_toolchain() {
  local need_major=1 need_minor=27
  local have_major=0 have_minor=0
  if command -v go >/dev/null 2>&1; then
    local version
    version="$(go env GOVERSION 2>/dev/null || go version | awk '{print $3}')"
    version="${version#go}"
    have_major="${version%%.*}"
    have_minor="${version#*.}"
    have_minor="${have_minor%%.*}"
  fi
  if [[ "$have_major" -gt "$need_major" ]] || {
    [[ "$have_major" -eq "$need_major" && "$have_minor" -ge "$need_minor" ]]
  }; then
    echo "go $(go version) already satisfies >= ${need_major}.${need_minor}"
    # Ports may still force GOTOOLCHAIN=local; keep the local toolchain once it is new enough.
    export GOTOOLCHAIN="${GOTOOLCHAIN:-local}"
    return 0
  fi

  local arch goarch
  arch="$(uname -m)"
  case "$arch" in
    x86_64 | amd64) goarch=amd64 ;;
    aarch64 | arm64) goarch=arm64 ;;
    *)
      echo "error: unsupported architecture for Go bootstrap: $arch" >&2
      return 1
      ;;
  esac
  local version="1.27.1"
  local archive="go${version}.openbsd-${goarch}.tar.gz"
  local url="https://go.dev/dl/${archive}"
  local expect_sha
  case "$goarch" in
    amd64) expect_sha="db0566b3b3ddf6eda09cb3434a9401eb2583ffa539475e45592812813f11c792" ;;
    arm64) expect_sha="953142ae3734098e65118ddca29ed2f469a85f039a78a1572da98ba271042e65" ;;
  esac
  local work
  work="$(mktemp -d /tmp/glifistore-go-boot.XXXXXX)"
  echo "installing Go ${version} from ${url}"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$work/$archive"
  elif command -v ftp >/dev/null 2>&1; then
    ftp -o "$work/$archive" "$url"
  else
    echo "error: curl or ftp required to bootstrap Go ${version}" >&2
    rm -rf "$work"
    return 1
  fi
  local actual_sha
  if command -v sha256 >/dev/null 2>&1; then
    actual_sha="$(sha256 -q "$work/$archive")"
  elif command -v sha256sum >/dev/null 2>&1; then
    actual_sha="$(sha256sum "$work/$archive" | awk '{print $1}')"
  else
    echo "error: sha256 or sha256sum required to verify Go ${version}" >&2
    rm -rf "$work"
    return 1
  fi
  if [[ "$actual_sha" != "$expect_sha" ]]; then
    echo "error: Go ${version} digest mismatch (got ${actual_sha}, want ${expect_sha})" >&2
    rm -rf "$work"
    return 1
  fi
  rm -rf /usr/local/go
  tar -C /usr/local -xzf "$work/$archive"
  rm -rf "$work"
  export PATH="/usr/local/go/bin:$PATH"
  # Prefer the bootstrapped toolchain over a ports GOTOOLCHAIN=local pin.
  export GOTOOLCHAIN=local
  hash -r 2>/dev/null || true
  go version
}

ensure_go_toolchain
command -v go >/dev/null 2>&1 || { echo "error: go required for TLS smoke" >&2; exit 1; }

echo "== Go TLS smoke (PUT→GET) =="
work="$(mktemp -d /tmp/glifistore-openbsd-tls.XXXXXX)"
cleanup() {
  cleanup_install
  if [[ -f "$work/daemon.pid" ]]; then
    kill "$(cat "$work/daemon.pid")" 2>/dev/null || true
    wait "$(cat "$work/daemon.pid")" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

# Mint a SAN-bearing leaf with a portable openssl.cnf (LibreSSL may lack -addext).
cat >"$work/openssl.cnf" <<'EOF'
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[req_distinguished_name]
CN = localhost

[v3_req]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost,IP:127.0.0.1
EOF

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$work/server.key" -out "$work/server.crt" -days 1 \
  -config "$work/openssl.cnf" >/dev/null 2>&1

mkdir -p "$root/sdk/go/bin"
# OpenBSD CI clones are often not a full git worktree from Go's POV; disable
# VCS stamping so `go build` does not fail with exit 128.
(cd "$root/sdk/go" && go build -buildvcs=false -o bin/glifistore-interop ./cmd/glifistore-interop)
go_helper="$root/sdk/go/bin/glifistore-interop"

"$daemon" --bind 127.0.0.1 --port 0 --workers 1 \
  --storage-mode volatile --executor-affinity \
  --tls-cert "$work/server.crt" --tls-key "$work/server.key" \
  >"$work/daemon.log" 2>&1 &
echo $! >"$work/daemon.pid"

port=""
for _ in $(jot 50 1); do
  if ! kill -0 "$(cat "$work/daemon.pid")" 2>/dev/null; then
    echo "error: glifistored exited early; log:" >&2
    cat "$work/daemon.log" >&2
    exit 1
  fi
  if grep -q 'transport=tls1.3 backend=LibreSSL' "$work/daemon.log" 2>/dev/null; then
    port="$(sed -n 's/.* port=\([0-9][0-9]*\) .*/\1/p' "$work/daemon.log" | head -1)"
    if [[ -n "$port" ]]; then
      break
    fi
  fi
  sleep 1
done
if [[ -z "$port" ]]; then
  echo "error: could not parse TLS listen port / LibreSSL backend from daemon log:" >&2
  cat "$work/daemon.log" >&2
  exit 1
fi
echo "daemon tls_port=$port backend=LibreSSL"
if ! grep -q 'openbsd-sandbox=pledge+unveil' "$work/daemon.log"; then
  echo "error: expected openbsd-sandbox=pledge+unveil in daemon log after Server::create" >&2
  cat "$work/daemon.log" >&2
  exit 1
fi
echo "openbsd sandbox applied"

key_hex="$(printf 'openbsd-tls-smoke' | od -An -tx1 | tr -d ' \n')"
value_hex="$(printf 'libressl-ok' | od -An -tx1 | tr -d ' \n')"
"$go_helper" --port "$port" --tls --tls-ca "$work/server.crt" --server-name localhost \
  --key-hex "$key_hex" --value-hex "$value_hex" put
got="$("$go_helper" --port "$port" --tls --tls-ca "$work/server.crt" --server-name localhost \
  --key-hex "$key_hex" get | tr -d '\n')"
if [[ "$got" != "$value_hex" ]]; then
  echo "error: TLS GET mismatch got='$got' want='$value_hex'" >&2
  exit 1
fi

echo "OpenBSD / LibreSSL CI gate OK"
