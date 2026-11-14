#!/usr/bin/env bash
# Durable daemon soak / stress entry point.
#
# Profiles (SOAK_PROFILE or --profile):
#   smoke               — default ~45s PUT/GET + reconnect + overwrite churn + drain (PR CI)
#   long                — 30 minutes (weekly schedule / local)
#   1h                  — 3600s multi-hour path with RSS + STATS sampling
#   4h                  — 14400s multi-hour path with RSS + STATS sampling
#   hot-key             — ~45s hammer one key (software stub; not hardware fairness cert)
#   connection-churn    — ~45s reconnect every op (software stub)
#   queue-saturation    — ~45s with tiny --durable-mutation-queue-capacity (expects OVERLOADED)
#   adversarial-reclaim — ~45s heavy overwrite churn (software stub; not reclaim-fairness cert)
#
# Or set SOAK_SECONDS / --seconds N directly (overrides profile duration).
#
# Honesty:
# - smoke and weekly 30m are CI-friendly evidence, not multi-hour hardware certification.
# - 1h/4h profiles sample RSS and STATS (rotation/compaction counters) when feasible;
#   rotation may still be zero depending on segment growth.
# - hot-key / connection-churn / queue-saturation / adversarial-reclaim are Wave 4 software
#   stubs that exercise shape and drain under stress. They do **not** close HAZ-026
#   multi-hour adversarial reclaim fairness, E3/E4 power-loss, or absolute hardware budgets.
# - End-of-run always uses SIGTERM + --shutdown-drain-ms so the durability boundary is the
#   graceful drain path (not SIGKILL). SIGTERM mid-mutation still leaves client ACK
#   indeterminate after commit — see docs/operations/soak.md and graceful-drain-and-overload.md.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefer_bins=(
  "$root/build/macos-debug"
  "$root/build/macos-release"
  "$root/build/macos-native-release"
  "$root/build/unix-debug"
  "$root/build/unix-release"
)

profile="${SOAK_PROFILE:-smoke}"
seconds_override="${SOAK_SECONDS:-}"
workers=1
storage_mode=durable-periodic
sample_every="${SOAK_SAMPLE_EVERY:-}"
rss_fail_factor="${SOAK_RSS_FAIL_FACTOR:-3}"
rss_fail_delta_kb="${SOAK_RSS_FAIL_DELTA_KB:-262144}" # 256 MiB
queue_capacity=""

profile_seconds() {
  case "$1" in
    smoke|hot-key|connection-churn|queue-saturation|adversarial-reclaim) echo 45 ;;
    long) echo 1800 ;;
    1h) echo 3600 ;;
    4h) echo 14400 ;;
    *)
      echo "unknown soak profile: $1 (expected smoke|long|1h|4h|hot-key|connection-churn|queue-saturation|adversarial-reclaim)" >&2
      return 2
      ;;
  esac
}

default_sample_every() {
  case "$1" in
    smoke|hot-key|connection-churn|queue-saturation|adversarial-reclaim) echo 0 ;;
    long) echo 60 ;;
    1h) echo 60 ;;
    4h) echo 120 ;;
    *) echo 0 ;;
  esac
}

is_adversarial_profile() {
  case "$1" in
    hot-key|connection-churn|queue-saturation|adversarial-reclaim) return 0 ;;
    *) return 1 ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds)
      seconds_override="$2"
      shift 2
      ;;
    --profile)
      profile="$2"
      shift 2
      ;;
    --workers)
      workers="$2"
      shift 2
      ;;
    --storage-mode)
      storage_mode="$2"
      shift 2
      ;;
    --sample-every)
      sample_every="$2"
      shift 2
      ;;
    -h|--help)
      cat <<EOF
Usage: $0 [--profile smoke|long|1h|4h|hot-key|connection-churn|queue-saturation|adversarial-reclaim]
          [--seconds N] [--workers N] [--storage-mode MODE] [--sample-every SEC]
Env: SOAK_PROFILE, SOAK_SECONDS, SOAK_SAMPLE_EVERY, GLIFISTORED,
     GLIFISTORE_INTEROP_CLIENT, SOAK_RSS_FAIL_FACTOR, SOAK_RSS_FAIL_DELTA_KB
EOF
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -n "$seconds_override" ]]; then
  seconds="$seconds_override"
else
  seconds="$(profile_seconds "$profile")"
fi
if [[ -z "$sample_every" ]]; then
  sample_every="$(default_sample_every "$profile")"
fi
if [[ "$profile" == "queue-saturation" ]]; then
  queue_capacity="${SOAK_DURABLE_MUTATION_QUEUE_CAPACITY:-2}"
fi

resolve_bin() {
  local name="$1"
  local override="$2"
  if [[ -n "$override" && -x "$override" ]]; then
    printf '%s\n' "$override"
    return 0
  fi
  for dir in "${prefer_bins[@]}"; do
    if [[ -x "$dir/$name" ]]; then
      printf '%s\n' "$dir/$name"
      return 0
    fi
  done
  return 1
}

discover_port() {
  local pid="$1"
  lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null |
    awk 'NR==2 {split($9,a,":"); print a[length(a)]}'
}

to_hex() {
  if command -v xxd >/dev/null 2>&1; then
    xxd -p -c 256 | tr -d '\n'
  else
    od -An -tx1 | tr -d ' \n'
  fi
}

rss_kb() {
  local pid="$1"
  # Portable: RSS in kilobytes (ps on macOS/Linux/OpenBSD).
  ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ' || echo 0
}

fetch_stats() {
  local port="$1"
  PYTHONPATH="$root/sdk/python/src${PYTHONPATH:+:$PYTHONPATH}" python3 - "$port" <<'PY'
import socket
import struct
import sys
from glifistore.protocol import Opcode, Status, encode_request, decode_response

port = int(sys.argv[1])
frame = encode_request(Opcode.STATS, 42)
with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
    sock.sendall(frame)
    buf = b""
    while len(buf) < 40:
        chunk = sock.recv(4096)
        if not chunk:
            raise SystemExit("short STATS response")
        buf += chunk
    frame_size = struct.unpack_from("<I", buf, 0)[0]
    while len(buf) < frame_size:
        chunk = sock.recv(frame_size - len(buf))
        if not chunk:
            break
        buf += chunk
    response = decode_response(buf)
    if response.status != Status.OK:
        raise SystemExit(f"STATS status={response.status}")
    sys.stdout.write(response.value.decode("ascii", errors="replace"))
PY
}

client_put() {
  local key_hex="$1"
  local value_hex="$2"
  local allow_overloaded="${3:-0}"
  local out
  set +e
  out="$("$client" --host 127.0.0.1 --port "$port" put --key-hex "$key_hex" --value-hex "$value_hex" 2>&1)"
  local rc=$?
  set -e
  if (( rc == 0 )); then
    return 0
  fi
  if (( allow_overloaded != 0 )) && [[ "$out" == *[Oo]verloaded* || "$out" == *OVERLOADED* ]]; then
    return 0
  fi
  echo "PUT failed: $out" >&2
  return 1
}

client_get() {
  local key_hex="$1"
  local allow_fail="${2:-0}"
  set +e
  "$client" --host 127.0.0.1 --port "$port" get --key-hex "$key_hex" >/dev/null 2>&1
  local rc=$?
  set -e
  if (( rc == 0 )); then
    return 0
  fi
  if (( allow_fail != 0 )); then
    return 0
  fi
  echo "GET failed for key_hex=$key_hex" >&2
  return 1
}

daemon="$(resolve_bin glifistored "${GLIFISTORED:-}" || true)"
client="$(resolve_bin glifistore_interop_client "${GLIFISTORE_INTEROP_CLIENT:-}" || true)"
if [[ -z "$daemon" || -z "$client" ]]; then
  echo "missing glifistored or glifistore_interop_client" >&2
  exit 1
fi
if ! command -v lsof >/dev/null 2>&1; then
  echo "lsof is required" >&2
  exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-soak-XXXXXX")"
daemon_pid=""
cleanup() {
  if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

data_dir="$work/data"
daemon_args=(
  --quiet --bind 127.0.0.1 --port 0 --workers "$workers"
  --storage-mode "$storage_mode" --data-dir "$data_dir" --open-mode create-new
  --shutdown-drain-ms 5000 --maintenance-mode background
)
if [[ -n "$queue_capacity" ]]; then
  daemon_args+=(--durable-mutation-queue-capacity "$queue_capacity")
fi
"$daemon" "${daemon_args[@]}" >"$work/daemon.out" 2>"$work/daemon.err" &
daemon_pid=$!

port=""
for _ in $(seq 1 100); do
  port="$(discover_port "$daemon_pid" || true)"
  if [[ -n "$port" ]]; then
    break
  fi
  if ! kill -0 "$daemon_pid" 2>/dev/null; then
    echo "soak daemon exited early:" >&2
    cat "$work/daemon.out" "$work/daemon.err" >&2 || true
    exit 1
  fi
  sleep 0.05
done
if [[ -z "$port" ]]; then
  echo "failed to discover soak daemon port" >&2
  exit 1
fi

rss0="$(rss_kb "$daemon_pid")"
echo "soak start profile=$profile seconds=$seconds workers=$workers storage=$storage_mode port=$port rss_kb=$rss0 sample_every=$sample_every queue_capacity=${queue_capacity:-default}"
deadline=$((SECONDS + seconds))
ops=0
reconnects=0
overloaded=0
samples=0
rss_peak="$rss0"
last_sample_at=$SECONDS
rotation_committed=0
useful_compactions=0
hot_key="soak-hot-key"
hot_key_hex="$(printf '%s' "$hot_key" | to_hex)"

while (( SECONDS < deadline )); do
  if ! kill -0 "$daemon_pid" 2>/dev/null; then
    echo "soak daemon died mid-run:" >&2
    cat "$work/daemon.out" "$work/daemon.err" >&2 || true
    exit 1
  fi

  case "$profile" in
    hot-key)
      value="hot-$ops-$(date +%s)"
      value_hex="$(printf '%s' "$value" | to_hex)"
      if ! client_put "$hot_key_hex" "$value_hex" 0; then
        exit 1
      fi
      if ! client_get "$hot_key_hex" 0; then
        exit 1
      fi
      ops=$((ops + 1))
      ;;
    connection-churn)
      key="churn-key-$ops"
      value="churn-value-$ops-$(date +%s)"
      key_hex="$(printf '%s' "$key" | to_hex)"
      value_hex="$(printf '%s' "$value" | to_hex)"
      if ! client_put "$key_hex" "$value_hex" 0; then
        exit 1
      fi
      if ! client_get "$key_hex" 0; then
        exit 1
      fi
      ops=$((ops + 1))
      reconnects=$((reconnects + 1))
      ;;
    queue-saturation)
      key="sat-key-$ops"
      value="sat-value-$ops-$(date +%s)-padpadpadpadpadpad"
      key_hex="$(printf '%s' "$key" | to_hex)"
      value_hex="$(printf '%s' "$value" | to_hex)"
      set +e
      out="$("$client" --host 127.0.0.1 --port "$port" put --key-hex "$key_hex" --value-hex "$value_hex" 2>&1)"
      rc=$?
      set -e
      if (( rc == 0 )); then
        ops=$((ops + 1))
        client_get "$key_hex" 1 || true
      elif [[ "$out" == *[Oo]verloaded* || "$out" == *OVERLOADED* ]]; then
        overloaded=$((overloaded + 1))
        ops=$((ops + 1))
      else
        echo "PUT failed under queue-saturation: $out" >&2
        exit 1
      fi
      ;;
    adversarial-reclaim)
      key="reclaim-key-$((ops % 8))"
      value="reclaim-value-$ops-$(date +%s)"
      key_hex="$(printf '%s' "$key" | to_hex)"
      value_hex="$(printf '%s' "$value" | to_hex)"
      if ! client_put "$key_hex" "$value_hex" 0; then
        exit 1
      fi
      if (( ops % 2 == 0 )); then
        overwrite_hex="$(printf 'adv-overwrite-%s' "$ops" | to_hex)"
        if ! client_put "$key_hex" "$overwrite_hex" 0; then
          exit 1
        fi
      fi
      if ! client_get "$key_hex" 0; then
        exit 1
      fi
      ops=$((ops + 1))
      if (( ops % 10 == 0 )); then
        reconnects=$((reconnects + 1))
      fi
      ;;
    *)
      key="soak-key-$ops"
      value="soak-value-$ops-$(date +%s)"
      key_hex="$(printf '%s' "$key" | to_hex)"
      value_hex="$(printf '%s' "$value" | to_hex)"
      if ! client_put "$key_hex" "$value_hex" 0; then
        exit 1
      fi
      if ! client_get "$key_hex" 0; then
        exit 1
      fi
      ops=$((ops + 1))
      # Periodic reconnect stress: open a fresh client path every 25 ops.
      if (( ops % 25 == 0 )); then
        reconnects=$((reconnects + 1))
        "$client" --host 127.0.0.1 --port "$port" get --key-hex "$key_hex" >/dev/null
      fi
      # Overwrite churn to create reclaimable sealed history for background vacuum.
      if (( ops % 7 == 0 )); then
        overwrite_hex="$(printf 'overwrite-%s' "$ops" | to_hex)"
        client_put "$key_hex" "$overwrite_hex" 0
      fi
      ;;
  esac

  if (( sample_every > 0 )) && (( SECONDS - last_sample_at >= sample_every )); then
    last_sample_at=$SECONDS
    samples=$((samples + 1))
    rss_now="$(rss_kb "$daemon_pid")"
    if (( rss_now > rss_peak )); then
      rss_peak="$rss_now"
    fi
    stats_text="$(fetch_stats "$port" || true)"
    if [[ -n "$stats_text" ]]; then
      rotation_committed="$(printf '%s\n' "$stats_text" | sed -n 's/^durable_rotations_committed=//p' | head -1)"
      useful_compactions="$(printf '%s\n' "$stats_text" | sed -n 's/^useful_compactions=//p' | head -1)"
      rotation_committed="${rotation_committed:-0}"
      useful_compactions="${useful_compactions:-0}"
    fi
    echo "soak sample t=${SECONDS}s ops=$ops rss_kb=$rss_now rss_peak_kb=$rss_peak rotations=$rotation_committed useful_compactions=$useful_compactions"
  fi
done

if [[ "$profile" == "queue-saturation" ]] && (( overloaded == 0 )); then
  echo "queue-saturation profile expected at least one OVERLOADED admission" >&2
  exit 1
fi

stats_final="$(fetch_stats "$port")"
required=(
  "lane[0].queue_wait_ns.count="
  "lane[0].service_ns.le_inf="
  "maintenance_rate_window_bytes_copied="
  "useful_compactions="
  "durable_rotations_committed="
)
for needle in "${required[@]}"; do
  if [[ "$stats_final" != *"$needle"* ]]; then
    echo "STATS missing $needle" >&2
    exit 1
  fi
done
rotation_committed="$(printf '%s\n' "$stats_final" | sed -n 's/^durable_rotations_committed=//p' | head -1)"
useful_compactions="$(printf '%s\n' "$stats_final" | sed -n 's/^useful_compactions=//p' | head -1)"
echo "soak STATS ok rotations=${rotation_committed:-0} useful_compactions=${useful_compactions:-0}"

rss_final="$(rss_kb "$daemon_pid")"
if (( rss_final > rss_peak )); then
  rss_peak="$rss_final"
fi

# Multi-hour profiles: fail closed on runaway RSS (factor AND absolute delta).
if [[ "$profile" == "1h" || "$profile" == "4h" ]] || (( seconds >= 3600 )); then
  if (( rss0 > 0 && rss_peak > rss0 * rss_fail_factor && rss_peak - rss0 > rss_fail_delta_kb )); then
    echo "soak RSS growth failed: rss0=$rss0 peak=$rss_peak factor>$rss_fail_factor delta_kb>$rss_fail_delta_kb" >&2
    exit 1
  fi
fi

# Graceful durability-boundary drain (never SIGKILL for routine soak teardown).
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=""
if is_adversarial_profile "$profile"; then
  echo "soak ok profile=$profile (adversarial software stub) ops=$ops reconnects=$reconnects overloaded=$overloaded seconds=$seconds samples=$samples rss_kb=$rss0->$rss_final peak=$rss_peak rotations=${rotation_committed:-0} useful_compactions=${useful_compactions:-0}"
else
  echo "soak ok profile=$profile ops=$ops reconnects=$reconnects seconds=$seconds samples=$samples rss_kb=$rss0->$rss_final peak=$rss_peak rotations=${rotation_committed:-0} useful_compactions=${useful_compactions:-0}"
fi
