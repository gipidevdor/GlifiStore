#!/usr/bin/env bash
# Exercise critical operator runbook paths against built tools + glifistored.
# Covers offline verify/backup/restore, corruption quarantine repair, and a short
# graceful drain with STATS histogram/rate-window checks. CI/staging smoke only.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefer_bins=(
  "$root/build/macos-debug"
  "$root/build/macos-release"
  "$root/build/macos-native-release"
  "$root/build/unix-debug"
  "$root/build/unix-release"
)

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

daemon="$(resolve_bin glifistored "${GLIFISTORED:-}" || true)"
verify="$(resolve_bin glifistore_verify_store "${GLIFISTORE_VERIFY_STORE:-}" || true)"
backup="$(resolve_bin glifistore_backup_store "${GLIFISTORE_BACKUP_STORE:-}" || true)"
repair="$(resolve_bin glifistore_repair_store "${GLIFISTORE_REPAIR_STORE:-}" || true)"
client="$(resolve_bin glifistore_interop_client "${GLIFISTORE_INTEROP_CLIENT:-}" || true)"

missing=0
for label in daemon:glifistored verify:glifistore_verify_store backup:glifistore_backup_store \
  repair:glifistore_repair_store client:glifistore_interop_client; do
  name="${label%%:*}"
  bin="${label#*:}"
  if [[ -z "${!name}" ]]; then
    echo "missing $bin; build a preset that produces it first" >&2
    missing=1
  fi
done
if [[ "$missing" -ne 0 ]]; then
  exit 1
fi
if ! command -v lsof >/dev/null 2>&1; then
  echo "lsof is required to discover ephemeral glifistored ports" >&2
  exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-ops-XXXXXX")"
cleanup() {
  if [[ -n "${daemon_pid:-}" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

source_dir="$work/source"
backup_dir="$work/backup"
restored_dir="$work/restored"
corrupt_dir="$work/corrupt"
repair_workspace="$work/repair-workspace"
daemon_dir="$work/daemon-data"
mkdir -p "$source_dir"

to_hex() {
  if command -v xxd >/dev/null 2>&1; then
    xxd -p -c 256 | tr -d '\n'
  else
    od -An -tx1 | tr -d ' \n'
  fi
}

start_daemon() {
  local data_dir="$1"
  local open_mode="$2"
  local log_file="$3"
  # Populate caller's daemon_pid / discovered_port (not a subshell).
  "$daemon" --quiet --bind 127.0.0.1 --port 0 --workers 1 \
    --storage-mode durable-sync --data-dir "$data_dir" --open-mode "$open_mode" \
    --shutdown-drain-ms 2000 --maintenance-mode cooperative \
    >"$log_file.out" 2>"$log_file.err" &
  daemon_pid=$!
  discovered_port=""
  for _ in $(seq 1 100); do
    discovered_port="$(discover_port "$daemon_pid" || true)"
    if [[ -n "$discovered_port" ]]; then
      return 0
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      echo "daemon exited early:" >&2
      cat "$log_file.out" "$log_file.err" >&2 || true
      daemon_pid=""
      return 1
    fi
    sleep 0.05
  done
  echo "failed to discover daemon port" >&2
  cat "$log_file.out" "$log_file.err" >&2 || true
  return 1
}

daemon_pid=""
discovered_port=""

echo "==> seed durable store via glifistored + interop client"
start_daemon "$daemon_dir" create-new "$work/daemon-seed"
port="$discovered_port"
key_hex="$(printf 'ops-runbook-key' | to_hex)"
value_hex="$(printf 'ops-runbook-value' | to_hex)"
"$client" --host 127.0.0.1 --port "$port" put --key-hex "$key_hex" --value-hex "$value_hex" >/dev/null
"$client" --host 127.0.0.1 --port "$port" get --key-hex "$key_hex" >/dev/null
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=""

cp -R "$daemon_dir/." "$source_dir/"

echo "==> verify (corruption runbook preflight)"
"$verify" -- "$source_dir"

echo "==> backup + restore (backup-restore runbook)"
"$backup" -- "$source_dir" "$backup_dir"
"$backup" -- "$backup_dir" "$restored_dir"
"$verify" -- "$restored_dir"

echo "==> corruption repair (quarantine orphan segment)"
mkdir -p "$corrupt_dir"
cp -R "$source_dir/." "$corrupt_dir/"
orphan="$corrupt_dir/segment-00000000000000ff-0000000a.glifi"
printf 'orphan-bytes' >"$orphan"
chmod 600 "$orphan"
"$repair" -- "$corrupt_dir" "$repair_workspace"
"$verify" -- "$repair_workspace/store"

echo "==> graceful drain smoke + STATS histogram export"
start_daemon "$daemon_dir" open-existing "$work/daemon-drain"
port="$discovered_port"
"$client" --host 127.0.0.1 --port "$port" get --key-hex "$key_hex" >/dev/null
PYTHONPATH="$root/sdk/python/src${PYTHONPATH:+:$PYTHONPATH}" python3 - "$port" <<'PY'
import socket
import struct
import sys
from glifistore.protocol import Opcode, Status, encode_request, decode_response

port = int(sys.argv[1])
frame = encode_request(Opcode.STATS, 1)
with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
    sock.sendall(frame)
    buf = b""
    while len(buf) < 40:
        chunk = sock.recv(4096)
        if not chunk:
            raise SystemExit("short STATS header")
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
    text = response.value.decode("ascii", errors="replace")
    if not text.startswith("GlifiStore/stats\n"):
        raise SystemExit("unexpected STATS banner")
    for needle in (
        "maintenance_rate_window_bytes_copied=",
        "maintenance_rate_window_cpu_ns=",
        "lane[0].queue_wait_ns.count=",
        "lane[0].service_ns.le_inf=",
    ):
        if needle not in text:
            raise SystemExit(f"STATS missing {needle}")
    print("STATS histogram + rate-window fields present")
PY
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=""

echo "ops runbook exercise: ok"
