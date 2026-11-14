#!/usr/bin/env bash
# Run one wire-v2 compatibility direction against sealed installed artifacts.
set -euo pipefail

candidate_prefix=""
prior_prefix=""
candidate_client=""
prior_client=""
check=""
work=""
daemon_pid=""

usage() {
  echo "usage: $0 --candidate-prefix DIR --prior-prefix DIR --candidate-client FILE --prior-client FILE --work DIR --check CHECK" >&2
}

while (($#)); do
  case "$1" in
    --candidate-prefix) candidate_prefix="${2:-}"; shift 2 ;;
    --prior-prefix) prior_prefix="${2:-}"; shift 2 ;;
    --candidate-client) candidate_client="${2:-}"; shift 2 ;;
    --prior-client) prior_client="${2:-}"; shift 2 ;;
    --work) work="${2:-}"; shift 2 ;;
    --check) check="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done
case "$check" in
  new-client-new-server|old-client-new-server|new-client-old-server) ;;
  *) echo "invalid --check: $check" >&2; usage; exit 2 ;;
esac
if [[ -z "$candidate_prefix" || -z "$prior_prefix" || -z "$candidate_client" ||
      -z "$prior_client" || -z "$work" ]]; then
  usage
  exit 2
fi
if ! command -v lsof >/dev/null 2>&1; then
  echo "lsof is required to discover the daemon's ephemeral port" >&2
  exit 1
fi
if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to enforce bounded client execution" >&2
  exit 1
fi
candidate_prefix="$(cd "$candidate_prefix" && pwd -P)"
prior_prefix="$(cd "$prior_prefix" && pwd -P)"
candidate_client="$(cd "$(dirname "$candidate_client")" && pwd -P)/$(basename "$candidate_client")"
prior_client="$(cd "$(dirname "$prior_client")" && pwd -P)/$(basename "$prior_client")"
mkdir -p "$work"
work="$(cd "$work" && pwd -P)"
for prefix in "$candidate_prefix" "$prior_prefix"; do
  test -x "$prefix/bin/glifistored"
done
for client in "$candidate_client" "$prior_client"; do
  test -x "$client"
done

cleanup() {
  if [[ -n "$daemon_pid" ]]; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
    daemon_pid=""
  fi
}
trap cleanup EXIT INT TERM

discover_port() {
  lsof -nP -iTCP -sTCP:LISTEN -a -p "$1" 2>/dev/null |
    awk 'NR==2 {split($9,a,":"); print a[length(a)]}'
}

run_client() {
  if ! perl -e 'alarm shift; exec @ARGV or die "exec failed: $!\n"' 10 "$@"; then
    echo "wire client failed or exceeded the 10-second deadline" >&2
    return 1
  fi
}

run_direction() {
  local server_prefix="$1"
  local client="$2"
  local label="$3"
  local daemon="$server_prefix/bin/glifistored"
  local log="$work/$label.daemon.log"
  "$daemon" --quiet --bind 127.0.0.1 --port 0 --workers 1 \
    --storage-mode volatile --shutdown-drain-ms 2000 --maintenance-mode cooperative \
    >"$log" 2>&1 &
  daemon_pid=$!
  local port=""
  for _ in $(seq 1 200); do
    port="$(discover_port "$daemon_pid" || true)"
    if [[ -n "$port" ]]; then
      break
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      echo "$label daemon exited before becoming ready" >&2
      cat "$log" >&2 || true
      return 1
    fi
    sleep 0.05
  done
  if [[ -z "$port" ]]; then
    echo "$label daemon did not become ready" >&2
    cat "$log" >&2 || true
    return 1
  fi
  local key_hex value_hex
  key_hex="$(printf 'wire-v2-%s-key' "$label" | od -An -v -tx1 | tr -d ' \n')"
  value_hex="$(printf 'wire-v2-%s-value' "$label" | od -An -v -tx1 | tr -d ' \n')"
  run_client "$client" --host 127.0.0.1 --port "$port" pipeline-put-get \
    --key-hex "$key_hex" --value-hex "$value_hex"
  run_client "$client" --host 127.0.0.1 --port "$port" erase --key-hex "$key_hex"
  run_client "$client" --host 127.0.0.1 --port "$port" expect-not-found --key-hex "$key_hex"
  kill -TERM "$daemon_pid"
  wait "$daemon_pid"
  daemon_pid=""
}

case "$check" in
  new-client-new-server)
    run_direction "$candidate_prefix" "$candidate_client" "$check" || exit 1
    ;;
  old-client-new-server)
    run_direction "$candidate_prefix" "$prior_client" "$check" || exit 1
    ;;
  new-client-old-server)
    run_direction "$prior_prefix" "$candidate_client" "$check" || exit 1
    ;;
esac

echo "WIRE-COMPAT $check PASSED"
