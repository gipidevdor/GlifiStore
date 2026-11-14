#!/usr/bin/env bash
# Exercise one persistence compatibility check using only an installed candidate and a tagged Store fixture.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix=""
fixture=""
client=""
check=""

usage() {
  echo "usage: $0 --prefix DIR --fixture DIR --client FILE --check CHECK" >&2
}

while (($#)); do
  case "$1" in
    --prefix) prefix="${2:-}"; shift 2 ;;
    --fixture) fixture="${2:-}"; shift 2 ;;
    --client) client="${2:-}"; shift 2 ;;
    --check) check="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

case "$check" in
  new-opens-old|fixture-recovery|migration|backup-verify-repair) ;;
  *) echo "invalid --check: $check" >&2; usage; exit 2 ;;
esac

if [[ -z "$prefix" || -z "$fixture" || -z "$client" ]]; then
  usage
  exit 2
fi
prefix="$(cd "$prefix" && pwd -P)"
fixture="$(cd "$fixture" && pwd -P)"
if [[ ! -x "$client" ]]; then
  echo "installed-package interop client is not executable: $client" >&2
  exit 1
fi
if ! command -v lsof >/dev/null 2>&1; then
  echo "lsof is required to discover the candidate daemon's ephemeral port" >&2
  exit 1
fi

daemon="$prefix/bin/glifistored"
verify="$prefix/bin/glifistore_verify_store"
backup="$prefix/bin/glifistore_backup_store"
migrate="$prefix/bin/glifistore_migrate_store"
repair="$prefix/bin/glifistore_repair_store"
for binary in "$daemon" "$verify" "$backup" "$migrate" "$repair"; do
  if [[ ! -x "$binary" ]]; then
    echo "candidate prefix is missing executable: $binary" >&2
    exit 1
  fi
done

candidate_version="$($daemon --version)"
candidate_version="${candidate_version#glifistored }"
python3 "$root/engineering/tools/persistence_fixture.py" validate \
  "$fixture" --before-version "$candidate_version" --repository "$root"

read_metadata() {
  python3 - "$fixture/STORE-FIXTURE.json" "$1" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1], encoding="utf-8"))[sys.argv[2]]
if not isinstance(value, (str, int)) or isinstance(value, bool):
    raise SystemExit("invalid scalar fixture metadata")
print(value)
PY
}

workers="$(read_metadata worker_count)"
key_hex="$(read_metadata key_hex)"
value_hex="$(read_metadata value_hex)"
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-persistence-compat.XXXXXX")"
daemon_pid=""
cleanup() {
  if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

copy_store() {
  local destination="$1"
  mkdir -p "$destination"
  cp -R "$fixture/store/." "$destination/"
}

discover_port() {
  local pid="$1"
  lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null |
    awk 'NR==2 {split($9,a,":"); print a[length(a)]}'
}

start_daemon() {
  local data_dir="$1"
  local worker_count="$2"
  local label="$3"
  "$daemon" --quiet --bind 127.0.0.1 --port 0 --workers "$worker_count" \
    --storage-mode durable-sync --data-dir "$data_dir" --open-mode open-existing \
    --shutdown-drain-ms 2000 --maintenance-mode cooperative \
    >"$work/$label.stdout" 2>"$work/$label.stderr" &
  daemon_pid=$!
  discovered_port=""
  for _ in $(seq 1 200); do
    discovered_port="$(discover_port "$daemon_pid" || true)"
    if [[ -n "$discovered_port" ]]; then
      return 0
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      echo "candidate daemon exited while opening fixture:" >&2
      cat "$work/$label.stdout" "$work/$label.stderr" >&2 || true
      daemon_pid=""
      return 1
    fi
    sleep 0.05
  done
  echo "candidate daemon did not become ready" >&2
  cat "$work/$label.stdout" "$work/$label.stderr" >&2 || true
  return 1
}

stop_daemon() {
  kill -TERM "$daemon_pid"
  wait "$daemon_pid"
  daemon_pid=""
}

expect_probe() {
  local port="$1"
  local actual
  actual="$($client --host 127.0.0.1 --port "$port" get --key-hex "$key_hex")"
  if [[ "$actual" != "$value_hex" ]]; then
    echo "fixture probe mismatch: expected $value_hex, got $actual" >&2
    return 1
  fi
}

open_and_probe() {
  local data_dir="$1"
  local worker_count="$2"
  local label="$3"
  start_daemon "$data_dir" "$worker_count" "$label"
  expect_probe "$discovered_port"
  stop_daemon
}

case "$check" in
  new-opens-old)
    store="$work/old-store"
    copy_store "$store"
    "$verify" -- "$store"
    open_and_probe "$store" "$workers" "new-opens-old"
    ;;
  fixture-recovery)
    store="$work/recovery-store"
    copy_store "$store"
    open_and_probe "$store" "$workers" "recovery-first"
    "$verify" -- "$store"
    open_and_probe "$store" "$workers" "recovery-second"
    ;;
  migration)
    source_store="$work/migration-source"
    destination_store="$work/migration-destination"
    copy_store "$source_store"
    if [[ "$workers" -eq 1 ]]; then target_workers=2; else target_workers=1; fi
    "$migrate" --workers "$target_workers" -- "$source_store" "$destination_store"
    "$verify" -- "$destination_store"
    open_and_probe "$destination_store" "$target_workers" "migration-open"
    ;;
  backup-verify-repair)
    source_store="$work/backup-source"
    backup_store="$work/backup-copy"
    restored_store="$work/restored-copy"
    corrupt_store="$work/corrupt-copy"
    repair_workspace="$work/repair-workspace"
    copy_store "$source_store"
    "$backup" -- "$source_store" "$backup_store"
    "$verify" -- "$backup_store"
    "$backup" -- "$backup_store" "$restored_store"
    "$verify" -- "$restored_store"
    open_and_probe "$restored_store" "$workers" "restore-open"
    mkdir -p "$corrupt_store"
    cp -R "$source_store/." "$corrupt_store/"
    printf 'orphan-bytes' >"$corrupt_store/segment-00000000000000ff-0000000a.glifi"
    chmod 600 "$corrupt_store/segment-00000000000000ff-0000000a.glifi"
    "$repair" -- "$corrupt_store" "$repair_workspace"
    "$verify" -- "$repair_workspace/store"
    open_and_probe "$repair_workspace/store" "$workers" "repair-open"
    ;;
esac

echo "PERSISTENCE-COMPAT $check PASSED fixture=$(read_metadata product_version) candidate=$candidate_version"
