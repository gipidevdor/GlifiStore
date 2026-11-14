#!/usr/bin/env bash
# Cross-SDK interoperability matrix against a real volatile glifistored.
# Runs cleartext by default, then an opt-in TLS 1.3 matrix (Phase 2.4) when the
# daemon was built with TLS and openssl is available. Erlang is included in both
# matrices when OTP/rebar3 are available. Ruby ships the same TLS train as peers.
#
# Default routing is FNV (plain GlifiStore/2 INIT). With INTEROP_KEYED=1 (default),
# also runs a smaller SipHash matrix (--worker-hash-seed) so every official SDK
# parses extended INIT and routes identically (ADR 0030).
# With INTEROP_SECURE=1 (default), also runs a secure-profile matrix: mTLS +
# --authz-map + pinned --worker-hash-seed (ADR 0020–0022 + 0030).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python="${PYTHON:-python3}"
perl="${PERL:-perl}"
daemon="${GLIFISTORED:-}"
cpp_client="${GLIFISTORE_INTEROP_CLIENT:-}"
use_installed="${GLIFISTORE_INTEROP_USE_INSTALLED:-0}"
if [[ "$use_installed" != "0" && "$use_installed" != "1" ]]; then
  echo "GLIFISTORE_INTEROP_USE_INSTALLED must be 0 or 1" >&2
  exit 1
fi
export GLIFISTORE_INTEROP_USE_INSTALLED="$use_installed"
export GLIFISTORE_SOURCE_ROOT="$root"

prefer_bins=(
  "$root/build/macos-debug"
  "$root/build/macos-release"
  "$root/build/macos-native-release"
  "$root/build/unix-release"
  "$root/build/unix-debug"
)

if [[ -z "$daemon" ]]; then
  for dir in "${prefer_bins[@]}"; do
    if [[ -x "$dir/glifistored" ]]; then
      daemon="$dir/glifistored"
      break
    fi
  done
fi
# Prefer a TLS-capable glifistored when available (Phase 2.4 matrix).
if [[ -z "${GLIFISTORED:-}" ]]; then
  for dir in "${prefer_bins[@]}"; do
    if [[ -x "$dir/glifistored" ]] && "$dir/glifistored" --help 2>&1 | grep -q -- '--tls-cert'; then
      daemon="$dir/glifistored"
      break
    fi
  done
fi
if [[ -z "$cpp_client" ]]; then
  # Prefer interop client from the same build tree as the chosen daemon when possible.
  daemon_dir="$(dirname "$daemon")"
  if [[ -x "$daemon_dir/glifistore_interop_client" ]]; then
    cpp_client="$daemon_dir/glifistore_interop_client"
  else
    for dir in "${prefer_bins[@]}"; do
      if [[ -x "$dir/glifistore_interop_client" ]]; then
        cpp_client="$dir/glifistore_interop_client"
        break
      fi
    done
  fi
fi

if [[ -z "$daemon" || ! -x "$daemon" ]]; then
  echo "missing glifistored; build a preset that produces it first" >&2
  exit 1
fi
if [[ -z "$cpp_client" || ! -x "$cpp_client" ]]; then
  echo "missing glifistore_interop_client; build target glifistore_interop_client first" >&2
  exit 1
fi

if [[ "$use_installed" != "1" ]]; then
  export PYTHONPATH="$root/sdk/python/src${PYTHONPATH:+:$PYTHONPATH}"
  export PERL5LIB="$root/sdk/perl/lib${PERL5LIB:+:$PERL5LIB}"
fi
py_helper="$root/scripts/sdk_interop_py.py"
pl_helper="$root/scripts/sdk_interop_perl.pl"
go_helper="${GLIFISTORE_GO_INTEROP:-}"
if [[ -z "$go_helper" || ! -x "$go_helper" ]]; then
  if [[ "$use_installed" == "1" ]]; then
    echo "installed-artifact mode requires GLIFISTORE_GO_INTEROP" >&2
    exit 1
  fi
  mkdir -p "$root/sdk/go/bin"
  (cd "$root/sdk/go" && "${GO:-go}" build -o bin/glifistore-interop ./cmd/glifistore-interop)
  go_helper="$root/sdk/go/bin/glifistore-interop"
fi
ruby_bin="${RUBY:-}"
ruby_ready=0
ruby_helper="$root/sdk/ruby/exe/glifistore-interop"
if [[ "${INTEROP_SKIP_RUBY:-0}" == "1" ]]; then
  echo "note: Ruby SDK interop skipped (INTEROP_SKIP_RUBY=1)" >&2
  ruby_bin=""
else
  if [[ -z "$ruby_bin" ]]; then
    if [[ -x "$HOME/.local/bin/mise" ]]; then
      ruby_bin="$("$HOME/.local/bin/mise" exec ruby@3.4 -- which ruby 2>/dev/null || true)"
    fi
    if [[ -z "$ruby_bin" ]] && command -v ruby >/dev/null 2>&1; then
      ruby_bin="$(command -v ruby)"
    fi
  fi
  if [[ -n "$ruby_bin" && -x "$ruby_bin" ]] && "$ruby_bin" -e 'v=RUBY_VERSION.split(".").map!(&:to_i); exit(v[0] > 3 || (v[0]==3 && v[1] >= 2) ? 0 : 1)' 2>/dev/null; then
    ruby_ready=1
    if [[ "$use_installed" != "1" ]]; then
      export RUBYLIB="$root/sdk/ruby/lib${RUBYLIB:+:$RUBYLIB}"
    fi
  else
    if [[ "${INTEROP_REQUIRE_RUBY:-0}" == "1" ]]; then
      echo "missing Ruby >= 3.2 for interop (set RUBY= or install via mise)" >&2
      exit 1
    fi
    if [[ -n "${RUBY:-}" && ! -x "${RUBY}" ]]; then
      echo "note: Ruby SDK interop skipped (RUBY points to missing binary: $RUBY)" >&2
    else
      echo "note: Ruby SDK interop skipped (need usable Ruby >= 3.2; set INTEROP_SKIP_RUBY=1 to silence)" >&2
    fi
    ruby_bin=""
  fi
fi
erlang_helper="${GLIFISTORE_ERLANG_INTEROP:-}"
erlang_ready=0
if [[ -z "$erlang_helper" ]]; then
  erlang_helper="$root/sdk/erlang/scripts/glifistore-interop.escript"
fi
if [[ "$use_installed" == "1" ]] && command -v erl >/dev/null 2>&1 && \
  command -v escript >/dev/null 2>&1; then
  erlang_ready=1
elif command -v erl >/dev/null 2>&1 && command -v rebar3 >/dev/null 2>&1; then
  (cd "$root/sdk/erlang" && rebar3 compile >/dev/null)
  erlang_ready=1
elif [[ "${INTEROP_REQUIRE_ERLANG:-0}" == "1" ]]; then
  echo "missing Erlang/OTP and rebar3 for interop (set INTEROP_REQUIRE_ERLANG=0 to skip)" >&2
  exit 1
else
  echo "note: Erlang SDK interop skipped (install OTP + rebar3 to include erlang in matrix)" >&2
fi
chmod +x "$ruby_helper" "$py_helper" "$pl_helper" 2>/dev/null || true
chmod +x "$erlang_helper" 2>/dev/null || true

if [[ "$use_installed" == "1" ]]; then
  for entry in "cpp:$cpp_client" "go:$go_helper"; do
    sdk_name="${entry%%:*}"
    sdk_path="${entry#*:}"
    if [[ "$sdk_path" == "$root/"* ]]; then
      echo "installed-artifact mode refuses source-tree $sdk_name binary: $sdk_path" >&2
      exit 1
    fi
  done
fi

# Optional TLS client flags populated by run_matrix_for_workers when mode=tls.
tls_args=()

echo "== wire golden fixtures =="
"$python" "$root/scripts/generate_wire_fixtures.py" --verify "$root/tests/fixtures"
for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$root/sdk/python/tests/fixtures/$fixture"; then
    echo "Python vendored fixture drift: $fixture (run scripts/sync-sdk-fixtures.sh)" >&2
    exit 1
  fi
  if ! cmp -s "$root/tests/fixtures/$fixture" "$root/sdk/perl/t/fixtures/$fixture"; then
    echo "Perl vendored fixture drift: $fixture (run scripts/sync-sdk-fixtures.sh)" >&2
    exit 1
  fi
  if ! cmp -s "$root/tests/fixtures/$fixture" "$root/sdk/go/testdata/$fixture"; then
    echo "Go vendored fixture drift: $fixture (run scripts/sync-sdk-fixtures.sh)" >&2
    exit 1
  fi
  if ! cmp -s "$root/tests/fixtures/$fixture" "$root/sdk/ruby/test/fixtures/$fixture"; then
    echo "Ruby vendored fixture drift: $fixture (run scripts/sync-sdk-fixtures.sh)" >&2
    exit 1
  fi
  if ! cmp -s "$root/tests/fixtures/$fixture" "$root/sdk/erlang/test/fixtures/$fixture"; then
    echo "Erlang vendored fixture drift: $fixture (run scripts/sync-sdk-fixtures.sh)" >&2
    exit 1
  fi
done
echo "wire fixtures OK"

to_hex() {
  if command -v xxd >/dev/null 2>&1; then
    xxd -p -c 256 | tr -d '\n'
  else
    od -An -tx1 | tr -d ' \n'
  fi
}

put_sdk() {
  local sdk="$1" port="$2" key_hex="$3" value_hex="$4" expire="${5:-0}"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp)
      "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" put --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire"
      ;;
    python)
      "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" put --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire"
      ;;
    perl)
      "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire" put
      ;;
    go)
      "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire" put
      ;;
    ruby)
      "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire" put
      ;;
    erlang)
      escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" --expire-at-ns "$expire" put
      ;;
    *)
      echo "unknown sdk $sdk" >&2
      return 1
      ;;
  esac
}

get_sdk() {
  local sdk="$1" port="$2" key_hex="$3"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" get --key-hex "$key_hex" ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" get --key-hex "$key_hex" ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" get ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" get ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" get ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" get ;;
    *)
      echo "unknown sdk: $sdk" >&2
      return 1
      ;;
  esac
}

erase_sdk() {
  local sdk="$1" port="$2" key_hex="$3"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" erase --key-hex "$key_hex" ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" erase --key-hex "$key_hex" ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" erase ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" erase ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" erase ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" erase ;;
    *)
      echo "unknown sdk: $sdk" >&2
      return 1
      ;;
  esac
}

pipeline_sdk() {
  local sdk="$1" port="$2" key_hex="$3" value_hex="$4"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" pipeline-put-get --key-hex "$key_hex" --value-hex "$value_hex" ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" pipeline-put-get --key-hex "$key_hex" --value-hex "$value_hex" ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" pipeline-put-get ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" pipeline-put-get ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" pipeline-put-get ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" --value-hex "$value_hex" pipeline-put-get ;;
    *) return 1 ;;
  esac
}

worker_pipelines_sdk() {
  local sdk="$1" port="$2" label_hex="$3"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" worker-pipelines-put-get --key-hex "$label_hex" ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" worker-pipelines-put-get --key-hex "$label_hex" ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$label_hex" worker-pipelines-put-get ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$label_hex" worker-pipelines-put-get ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$label_hex" worker-pipelines-put-get ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$label_hex" worker-pipelines-put-get ;;
    *) return 1 ;;
  esac
}

expect_get() {
  local sdk="$1" port="$2" key_hex="$3" want_hex="$4"
  local got
  got="$(get_sdk "$sdk" "$port" "$key_hex" | tr -d '\n')"
  if [[ "$got" != "$want_hex" ]]; then
    echo "GET mismatch ($sdk): got='$got' want='$want_hex'" >&2
    return 1
  fi
}

expect_not_found_sdk() {
  local sdk="$1" port="$2" key_hex="$3"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" expect-not-found --key-hex "$key_hex" ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-not-found --key-hex "$key_hex" ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" expect-not-found ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" expect-not-found ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" expect-not-found ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" --key-hex "$key_hex" expect-not-found ;;
    *) return 1 ;;
  esac
}

expect_frame_limit_sdk() {
  local sdk="$1" port="$2"
  local extra=()
  if [[ ${#tls_args[@]} -gt 0 ]]; then
    extra=("${tls_args[@]}")
  fi
  case "$sdk" in
    cpp) "$cpp_client" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    python) "$python" "$py_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    perl) "$perl" "$pl_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    go) "$go_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    ruby) "$ruby_bin" "$ruby_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    erlang) escript "$erlang_helper" --port "$port" "${extra[@]+"${extra[@]}"}" expect-frame-limit ;;
    *) return 1 ;;
  esac
}

make_tls_material() {
  local directory="$1"
  if ! command -v openssl >/dev/null 2>&1; then
    return 1
  fi
  # CA + leaf (not a self-signed leaf-as-CA). OTP ssl and other strict
  # verifiers reject a leaf trust anchor that lacks CA:TRUE.
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$directory/ca.key" -out "$directory/ca.crt" -days 1 \
    -subj "/CN=glifistore-interop-tls-ca" >/dev/null 2>&1 || return 1
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$directory/server.key" -out "$directory/server.csr" \
    -subj "/CN=localhost" >/dev/null 2>&1 || return 1
  if printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' >"$directory/server.ext" &&
      openssl x509 -req -in "$directory/server.csr" -CA "$directory/ca.crt" -CAkey "$directory/ca.key" \
        -CAcreateserial -out "$directory/server.crt" -days 1 \
        -extfile "$directory/server.ext" >/dev/null 2>&1; then
    return 0
  fi
  # Fallback without SAN extensions (older LibreSSL / openssl).
  openssl x509 -req -in "$directory/server.csr" -CA "$directory/ca.crt" -CAkey "$directory/ca.key" \
    -CAcreateserial -out "$directory/server.crt" -days 1 >/dev/null 2>&1
}

# CA + server + client PEMs and authz map for --secure-profile (principal CN=interop.writer).
make_secure_material() {
  local directory="$1"
  if ! command -v openssl >/dev/null 2>&1; then
    return 1
  fi
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$directory/ca.key" -out "$directory/ca.crt" -days 1 \
    -subj "/CN=glifistore-interop-ca" >/dev/null 2>&1 || return 1
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$directory/server.key" -out "$directory/server.csr" \
    -subj "/CN=localhost" >/dev/null 2>&1 || return 1
  printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' >"$directory/server.ext"
  openssl x509 -req -in "$directory/server.csr" -CA "$directory/ca.crt" -CAkey "$directory/ca.key" \
    -CAcreateserial -out "$directory/server.crt" -days 1 \
    -extfile "$directory/server.ext" >/dev/null 2>&1 || return 1
  # Client: CN only (no SAN) so principal id is exactly interop.writer.
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$directory/client.key" -out "$directory/client.csr" \
    -subj "/CN=interop.writer" >/dev/null 2>&1 || return 1
  openssl x509 -req -in "$directory/client.csr" -CA "$directory/ca.crt" -CAkey "$directory/ca.key" \
    -CAcreateserial -out "$directory/client.crt" -days 1 >/dev/null 2>&1 || return 1
  cat >"$directory/authz.map" <<'EOF'
interop.writer write
EOF
}

daemon_supports_tls() {
  # Help always advertises --tls-cert even when the binary was built without TLS.
  # Probe by requesting TLS config: TLS-off builds fail closed with a distinct
  # diagnostic before listening.
  local probe
  probe="$("$daemon" --dump-config --tls-cert /dev/null --tls-key /dev/null 2>&1 || true)"
  if grep -q 'built without TLS' <<<"$probe"; then
    return 1
  fi
  "$daemon" --help 2>&1 | grep -q -- '--tls-cert'
}

start_server() {
  local workers="$1"
  local port_file="$2"
  local log_file="$3"
  shift 3
  local resolved
  resolved="$("$daemon" --dump-config --workers "$workers" --max-connections 4096)"
  if { ! grep -qx "workers=$workers" <<<"$resolved" &&
      ! grep -qx "shard-pairs=$workers" <<<"$resolved"; } ||
      ! grep -qx "max-connections=4096" <<<"$resolved"; then
    echo "glifistored configuration mismatch for workers=$workers" >&2
    echo "$resolved" >&2
    return 1
  fi
  "$daemon" --bind 127.0.0.1 --port 0 --workers "$workers" \
    --storage-mode volatile --executor-affinity --quiet \
    "$@" \
    >"$log_file" 2>&1 &
  local pid=$!
  echo "$pid" >"${port_file}.pid"
  local port=""
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "glifistored exited early; see $log_file" >&2
      return 1
    fi
    port="$(lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null | awk 'NR==2 {split($9,a,":"); print a[length(a)]}')"
    if [[ -n "$port" ]]; then
      echo "$port" >"$port_file"
      return 0
    fi
    sleep 0.1
  done
  echo "could not discover glifistored listen port" >&2
  return 1
}

stop_server() {
  local port_file="$1"
  if [[ -f "${port_file}.pid" ]]; then
    local pid
    pid="$(cat "${port_file}.pid")"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "${port_file}.pid" "$port_file"
  fi
}

active_port_file=""
active_work=""
cleanup_active_run() {
  if [[ -n "$active_port_file" ]]; then
    stop_server "$active_port_file"
  fi
  if [[ -n "$active_work" ]]; then
    rm -rf "$active_work"
  fi
}
trap cleanup_active_run EXIT

run_matrix_for_workers() {
  local workers="$1"
  local mode="${2:-cleartext}" # cleartext | tls | secure
  local routing="${3:-fnv}"    # fnv | keyed
  local work
  work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-interop.XXXXXX")"
  local port_file="$work/port"
  local log_file="$work/server.log"
  active_port_file="$port_file"
  active_work="$work"
  local server_extra=()
  tls_args=()

  local writers=(cpp python perl go)
  local readers=(cpp python perl go)
  if [[ "$ruby_ready" == "1" ]]; then
    writers+=(ruby)
    readers+=(ruby)
  fi
  if [[ "$erlang_ready" == "1" ]]; then
    # Erlang ships TLS 1.3 (Phase 2); include in both cleartext and TLS matrices.
    writers+=(erlang)
    readers+=(erlang)
  fi

  if [[ "$mode" == "secure" ]]; then
    # Secure profile always selects siphash24-v1; pin seed for deterministic owners.
    routing=keyed
  fi

  echo "== interop mode=$mode routing=$routing workers=$workers =="
  if [[ "$mode" == "tls" ]]; then
    if ! make_tls_material "$work"; then
      echo "skipping TLS interop: openssl could not mint a test certificate" >&2
      rm -rf "$work"
      return 0
    fi
    server_extra=(--tls-cert "$work/server.crt" --tls-key "$work/server.key")
    tls_args=(--tls --tls-ca "$work/ca.crt" --server-name localhost)
  elif [[ "$mode" == "secure" ]]; then
    if ! make_secure_material "$work"; then
      echo "skipping secure-profile interop: openssl could not mint mTLS material" >&2
      rm -rf "$work"
      return 0
    fi
    server_extra=(
      --secure-profile
      --tls-cert "$work/server.crt"
      --tls-key "$work/server.key"
      --tls-client-ca "$work/ca.crt"
      --authz-map "$work/authz.map"
    )
    tls_args=(
      --tls
      --tls-ca "$work/ca.crt"
      --tls-cert "$work/client.crt"
      --tls-key "$work/client.key"
      --server-name localhost
    )
  fi

  if [[ "$mode" == "tls" || "$mode" == "secure" ]]; then
    if ! "$perl" -MIO::Socket::SSL -e1 >/dev/null 2>&1; then
      echo "  note: Perl without IO::Socket::SSL — excluding perl from TLS/secure matrix"
      local filtered_w=()
      local filtered_r=()
      local sdk
      for sdk in "${writers[@]}"; do
        [[ "$sdk" == "perl" ]] && continue
        filtered_w+=("$sdk")
      done
      for sdk in "${readers[@]}"; do
        [[ "$sdk" == "perl" ]] && continue
        filtered_r+=("$sdk")
      done
      writers=("${filtered_w[@]}")
      readers=("${filtered_r[@]}")
    fi
  fi

  if [[ "$routing" == "keyed" ]]; then
    # Fixed seed so every SDK must agree on SipHash ownership (ADR 0030).
    server_extra+=(--worker-hash-seed "${INTEROP_WORKER_HASH_SEED:-13957458623937596}")
  fi

  if [[ ${#server_extra[@]} -gt 0 ]]; then
    if ! start_server "$workers" "$port_file" "$log_file" "${server_extra[@]}"; then
      echo "secure/tls server start failed; log:" >&2
      cat "$log_file" >&2 || true
      rm -rf "$work"
      return 1
    fi
  else
    if ! start_server "$workers" "$port_file" "$log_file"; then
      echo "cleartext server start failed; log:" >&2
      cat "$log_file" >&2 || true
      rm -rf "$work"
      return 1
    fi
  fi
  local port
  port="$(cat "$port_file")"

  local case_id=0
  for writer in "${writers[@]}"; do
    for reader in "${readers[@]}"; do
      case_id=$((case_id + 1))
      local key_hex value_hex
      key_hex="$(printf 'interop-%s-%s-%02d-w%d\x00\xff' "$mode" "$routing" "$case_id" "$workers" | to_hex)"
      value_hex="$(printf 'val-%s-%s-%s-%s-\x00\xff' "$mode" "$routing" "$writer" "$reader" | to_hex)"
      echo "  $writer PUT → $reader GET (binary)"
      put_sdk "$writer" "$port" "$key_hex" "$value_hex"
      expect_get "$reader" "$port" "$key_hex" "$value_hex"
    done
  done

  for writer in "${writers[@]}"; do
    local erase_key erase_val
    erase_key="$(printf 'erase-%s-%s-%s-w%d\x00' "$mode" "$routing" "$writer" "$workers" | to_hex)"
    erase_val="$(printf 'erase-val-%s-\xff' "$writer" | to_hex)"
    echo "  $writer PUT → ERASE → structured NOT_FOUND"
    put_sdk "$writer" "$port" "$erase_key" "$erase_val"
    erase_sdk "$writer" "$port" "$erase_key"
    expect_not_found_sdk "$writer" "$port" "$erase_key"
    # Cross-SDK visibility of the tombstone (first reader that is not the writer, if any).
    for reader in "${readers[@]}"; do
      [[ "$reader" == "$writer" ]] && continue
      expect_not_found_sdk "$reader" "$port" "$erase_key"
      break
    done
  done

  local empty_key
  empty_key="$(printf 'empty-%s-%s-w%d' "$mode" "$routing" "$workers" | to_hex)"
  echo "  python PUT empty value → cross-SDK GET"
  put_sdk python "$port" "$empty_key" ""
  for sdk in "${readers[@]}"; do
    [[ "$sdk" == python ]] && continue
    expect_get "$sdk" "$port" "$empty_key" ""
  done

  local hash_seed="${INTEROP_WORKER_HASH_SEED:-13957458623937596}"
  while IFS=: read -r owner route_key; do
    local route_value
    route_value="$(printf 'owner-%s-%s-of-%s' "$routing" "$owner" "$workers" | to_hex)"
    echo "  cpp PUT → python GET (deterministic owner $owner)"
    put_sdk cpp "$port" "$route_key" "$route_value"
    expect_get python "$port" "$route_key" "$route_value"
  done < <("$python" - "$workers" "$routing" "$hash_seed" <<'PY'
import sys

from glifistore.protocol import (
    ROUTING_ALG_SIPHASH24_V1,
    WorkerRouting,
    hash_key_routing,
)

workers = int(sys.argv[1])
routing = sys.argv[2]
seed = int(sys.argv[3])
state = (
    WorkerRouting(algorithm=ROUTING_ALG_SIPHASH24_V1, seed=seed)
    if routing == "keyed"
    else WorkerRouting()
)
found = {}
candidate = 0
while len(found) < workers:
    key = f"route-{routing}-w{workers}-{candidate}".encode()
    found.setdefault(hash_key_routing(key, state) % workers, key.hex())
    candidate += 1
for owner in range(workers):
    print(f"{owner}:{found[owner]}")
PY
)

  for sdk in "${writers[@]}"; do
    local pkey pval
    pkey="$(printf 'pipe-%s-%s-%s-w%d' "$mode" "$routing" "$sdk" "$workers" | to_hex)"
    pval="$(printf 'pipe-val-\xff' | to_hex)"
    echo "  $sdk pipeline PUT/GET"
    local got
    got="$(pipeline_sdk "$sdk" "$port" "$pkey" "$pval" | tr -d '\n')"
    if [[ "$got" != "$pval" ]]; then
      echo "pipeline mismatch for $sdk" >&2
      stop_server "$port_file"
      rm -rf "$work"
      return 1
    fi
  done

  for sdk in "${writers[@]}"; do
    local wp_label
    wp_label="$(printf 'wp-%s-%s-%s-w%d' "$mode" "$routing" "$sdk" "$workers" | to_hex)"
    echo "  $sdk execute_worker_pipelines PUT/GET"
    local wp_got
    wp_got="$(worker_pipelines_sdk "$sdk" "$port" "$wp_label" | tr -d '\n')"
    if [[ "$wp_got" != "ok" ]]; then
      echo "worker pipelines mismatch for $sdk: got='$wp_got'" >&2
      stop_server "$port_file"
      rm -rf "$work"
      return 1
    fi
  done

  if [[ "$mode" == "cleartext" || "$mode" == "secure" ]]; then
    local missing_key exp_key exp_val expire_at
    missing_key="$(printf 'missing-%s-%s-w%d' "$mode" "$routing" "$workers" | to_hex)"
    for sdk in "${readers[@]}"; do
      echo "  $sdk structured NOT_FOUND"
      expect_not_found_sdk "$sdk" "$port" "$missing_key"
    done
    for sdk in "${writers[@]}"; do
      echo "  $sdk local 2MiB frame-limit rejection"
      expect_frame_limit_sdk "$sdk" "$port"
    done

    exp_key="$(printf 'exp-%s-w%d' "$routing" "$workers" | to_hex)"
    exp_val="$(printf 'soon' | to_hex)"
    expire_at="$("$python" - <<'PY'
import time
print(int((time.time() + 0.4) * 1_000_000_000))
PY
)"
    echo "  cpp PUT with expiry → go GET then miss"
    put_sdk cpp "$port" "$exp_key" "$exp_val" "$expire_at"
    expect_get go "$port" "$exp_key" "$exp_val"
    sleep 0.6
    if [[ " ${readers[*]} " == *" perl "* ]]; then
      expect_not_found_sdk perl "$port" "$exp_key"
    else
      expect_not_found_sdk python "$port" "$exp_key"
    fi
  fi

  stop_server "$port_file"
  rm -rf "$work"
  active_port_file=""
  active_work=""
  echo "mode=$mode routing=$routing workers=$workers OK"
}

workers_list=(1 2 4 8)
if [[ "${INTEROP_WORKERS:-}" != "" ]]; then
  # shellcheck disable=SC2206
  workers_list=(${INTEROP_WORKERS})
fi

if [[ "${INTEROP_SKIP_CLEARTEXT:-0}" == "1" ]]; then
  echo "cleartext FNV interop skipped (INTEROP_SKIP_CLEARTEXT=1)"
else
  for w in "${workers_list[@]}"; do
    run_matrix_for_workers "$w" cleartext fnv
  done
fi

if [[ "${INTEROP_KEYED:-1}" == "1" ]]; then
  keyed_workers=(2 4)
  if [[ "${INTEROP_KEYED_WORKERS:-}" != "" ]]; then
    # shellcheck disable=SC2206
    keyed_workers=(${INTEROP_KEYED_WORKERS})
  fi
  for w in "${keyed_workers[@]}"; do
    run_matrix_for_workers "$w" cleartext keyed
  done
else
  echo "keyed SipHash interop skipped (INTEROP_KEYED=0)"
fi

if [[ "${INTEROP_SKIP_TLS:-0}" == "1" ]]; then
  echo "TLS interop skipped (INTEROP_SKIP_TLS=1)"
elif ! daemon_supports_tls; then
  echo "TLS interop skipped (daemon built without --tls-cert)"
else
  # Keep TLS matrix smaller by default; INTEROP_TLS_WORKERS overrides.
  tls_workers=(1 2)
  if [[ "${INTEROP_TLS_WORKERS:-}" != "" ]]; then
    # shellcheck disable=SC2206
    tls_workers=(${INTEROP_TLS_WORKERS})
  fi
  for w in "${tls_workers[@]}"; do
    run_matrix_for_workers "$w" tls fnv
  done
fi

if [[ "${INTEROP_SECURE:-1}" != "1" ]]; then
  echo "secure-profile interop skipped (INTEROP_SECURE=0)"
elif ! daemon_supports_tls; then
  echo "secure-profile interop skipped (daemon built without --tls-cert)"
else
  secure_workers=(2)
  if [[ "${INTEROP_SECURE_WORKERS:-}" != "" ]]; then
    # shellcheck disable=SC2206
    secure_workers=(${INTEROP_SECURE_WORKERS})
  fi
  for w in "${secure_workers[@]}"; do
    run_matrix_for_workers "$w" secure keyed
  done
fi

echo "SDK interoperability matrix PASSED"
