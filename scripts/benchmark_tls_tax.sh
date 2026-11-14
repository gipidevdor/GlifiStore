#!/usr/bin/env bash
# Measure cleartext vs TLS 1.3 tax on the Go SDK pipeline harness (Phase 2.5).
#
# Same workload as scripts/benchmark_go_client.sh, but a focused matrix:
# workers={1,4} × pipeline={1,32,128} × transport={cleartext,tls1.3}.
#
# Usage:
#   ./scripts/benchmark_tls_tax.sh [outdir]
# Env:
#   GLIFISTORED OPS WARMUP REPEATS GO
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date -u +%Y%m%d-%H%M%S)"
sdk_version="$(tr -d '[:space:]' <"$root/VERSION")"
outdir="${1:-$root/benchmark-results-tls-tax-${sdk_version}-${stamp}}"
daemon="${GLIFISTORED:-}"
go_bin="${GO:-go}"
host="127.0.0.1"
ops="${OPS:-50000}"
warmup="${WARMUP:-1}"
repeats="${REPEATS:-5}"

prefer_bins=(
  "$root/build/macos-native-release"
  "$root/build/macos-release"
  "$root/build/macos-debug"
  "$root/build/unix-release"
  "$root/build/unix-debug"
)
if [[ -z "$daemon" ]]; then
  for dir in "${prefer_bins[@]}"; do
    if [[ -x "$dir/glifistored" ]] && "$dir/glifistored" --help 2>&1 | grep -q -- '--tls-cert'; then
      daemon="$dir/glifistored"
      break
    fi
  done
fi
if [[ -z "$daemon" || ! -x "$daemon" ]]; then
  echo "missing TLS-capable glifistored; build a preset with OpenSSL/LibreSSL first" >&2
  exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
  echo "openssl CLI required to mint ephemeral TLS material" >&2
  exit 1
fi

mkdir -p "$outdir/cleartext" "$outdir/tls" "$outdir/logs" "$outdir/certs"
mkdir -p "$root/sdk/go/bin"
(cd "$root/sdk/go" && "$go_bin" build -o bin/glifistore-bench ./cmd/glifistore-bench)
bench="$root/sdk/go/bin/glifistore-bench"

cat >"$outdir/certs/openssl.cnf" <<'EOF'
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
  -keyout "$outdir/certs/server.key" -out "$outdir/certs/server.crt" -days 1 \
  -config "$outdir/certs/openssl.cnf" >/dev/null 2>&1

{
  echo "captured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git -C "$root" rev-parse HEAD)"
  echo "go=$($go_bin version)"
  echo "go_sdk_version=$sdk_version"
  echo "glifistored=$daemon"
  echo "ops=$ops warmup=$warmup repeats=$repeats"
  echo "workload=ordered PUT/GET pipeline read-after-write, value_size=64"
  echo "storage_mode=volatile"
  echo "transports=cleartext,tls1.3"
} >"$outdir/environment.txt"

discover_port() {
  local pid="$1"
  local port=""
  if command -v lsof >/dev/null 2>&1; then
    for _ in $(seq 1 50); do
      port="$(lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null | awk 'NR==2 {split($9,a,":"); print a[length(a)]}')"
      if [[ -n "$port" ]]; then
        echo "$port"
        return 0
      fi
      sleep 0.1
    done
  fi
  return 1
}

start_server() {
  local workers="$1" port_file="$2" log_file="$3"
  shift 3
  "$daemon" --bind "$host" --port 0 --workers "$workers" \
    --storage-mode volatile --executor-affinity \
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
    port="$(discover_port "$pid" || true)"
    if [[ -z "$port" ]]; then
      port="$(sed -n 's/.* port=\([0-9][0-9]*\) .*/\1/p' "$log_file" | head -1)"
    fi
    if [[ -z "$port" ]]; then
      port="$(sed -n 's/.* cleartext_port=\([0-9][0-9]*\) .*/\1/p' "$log_file" | head -1)"
    fi
    if [[ -n "$port" ]]; then
      echo "$port" >"$port_file"
      return 0
    fi
    sleep 0.1
  done
  echo "could not discover glifistored listen port; see $log_file" >&2
  return 1
}

stop_server() {
  local port_file="$1"
  if [[ -f "${port_file}.pid" ]]; then
    kill "$(cat "${port_file}.pid")" 2>/dev/null || true
    wait "$(cat "${port_file}.pid")" 2>/dev/null || true
    rm -f "${port_file}.pid" "$port_file"
  fi
}

cleanup() {
  for pidfile in "$outdir"/logs/*.pid; do
    [[ -f "$pidfile" ]] || continue
    kill "$(cat "$pidfile")" 2>/dev/null || true
  done
}
trap cleanup EXIT

parse_median_ops() {
  local file="$1"
  sed -n 's/.*median_ops_per_second=\([0-9.][0-9.]*\).*/\1/p' "$file" | head -1
}

workers=(1 4)
pipelines=(1 32 128)
summary="$outdir/summary.md"
{
  echo "# TLS tax (Go client pipeline)"
  echo
  echo "- SDK version: \`$sdk_version\`"
  echo "- Ops (PUT/GET pairs): \`$ops\`"
  echo "- Warmup/repeats: \`$warmup\` / \`$repeats\`"
  echo "- Execution: concurrent"
  echo
  echo "| workers | pipeline | cleartext ops/s | TLS ops/s | TLS/cleartext |"
  echo "| ---: | ---: | ---: | ---: | ---: |"
} >"$summary"

for w in "${workers[@]}"; do
  clear_port_file="$outdir/logs/cleartext-w${w}.port"
  clear_log="$outdir/logs/cleartext-w${w}.log"
  tls_port_file="$outdir/logs/tls-w${w}.port"
  tls_log="$outdir/logs/tls-w${w}.log"

  start_server "$w" "$clear_port_file" "$clear_log"
  clear_port="$(cat "$clear_port_file")"
  start_server "$w" "$tls_port_file" "$tls_log" \
    --tls-cert "$outdir/certs/server.crt" --tls-key "$outdir/certs/server.key"
  tls_port="$(cat "$tls_port_file")"

  for p in "${pipelines[@]}"; do
    label="w${w}-p${p}"
    echo "running cleartext concurrent $label"
    "$bench" --host "$host" --port "$clear_port" --workers "$w" --ops "$ops" \
      --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution concurrent \
      | tee "$outdir/cleartext/concurrent-${label}.txt"

    echo "running tls concurrent $label"
    "$bench" --host "$host" --port "$tls_port" --workers "$w" --ops "$ops" \
      --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution concurrent \
      --tls --tls-ca "$outdir/certs/server.crt" --server-name localhost \
      | tee "$outdir/tls/concurrent-${label}.txt"

    clear_ops="$(parse_median_ops "$outdir/cleartext/concurrent-${label}.txt")"
    tls_ops="$(parse_median_ops "$outdir/tls/concurrent-${label}.txt")"
    ratio="n/a"
    if [[ -n "$clear_ops" && -n "$tls_ops" ]]; then
      ratio="$(awk -v t="$tls_ops" -v c="$clear_ops" 'BEGIN { if (c > 0) printf "%.3f", t/c; else print "n/a" }')"
    fi
    echo "| $w | $p | ${clear_ops:-n/a} | ${tls_ops:-n/a} | $ratio |" >>"$summary"
  done

  stop_server "$clear_port_file"
  stop_server "$tls_port_file"
done

{
  echo
  echo "Raw outputs: \`cleartext/\` and \`tls/\`. Ratio < 1.0 means TLS is slower."
  echo "Interpret on a quiet host; see docs/security/tls-performance.md."
} >>"$summary"

echo "wrote $summary"
