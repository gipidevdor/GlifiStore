#!/usr/bin/env bash
# Serialized full local benchmark campaign runner.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/benchmarks/results/local-macos-2026-08-30-full-b971a15}"
COMMANDS="$OUT/commands.txt"
LOG="$OUT/campaign.log"
PROGRESS="$OUT/progress.txt"
STATUS="$OUT/runner.status"

if [[ ! -f "$COMMANDS" ]]; then
  echo "missing $COMMANDS" >&2
  exit 2
fi

total=$(grep -c . "$COMMANDS" || true)
n=0
fail=0
skip=0
ok=0
echo "running=1 pid=$$ started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STATUS"

while IFS=$'\t' read -r dest cmd; do
  [[ -z "${dest:-}" || -z "${cmd:-}" ]] && continue
  n=$((n + 1))
  if [[ -s "$dest" ]]; then
    skip=$((skip + 1))
    echo "[$n/$total] SKIP existing $dest" >> "$PROGRESS"
    continue
  fi
  mkdir -p "$(dirname "$dest")"
  echo "[$n/$total] $cmd" | tee -a "$LOG" >> "$PROGRESS"
  set +e
  # Intentionally unquoted expansion of known command lines from commands.txt.
  eval "$cmd" >"$dest" 2>"$dest.err"
  rc=$?
  set -u
  if [[ $rc -ne 0 ]]; then
    echo "FAILED exit=$rc -> $dest" | tee -a "$LOG" >> "$PROGRESS"
    fail=$((fail + 1))
  else
    if [[ ! -s "$dest.err" ]]; then
      rm -f "$dest.err"
    fi
    if [[ ! -s "$dest" ]]; then
      echo "FAILED empty-output -> $dest" | tee -a "$LOG" >> "$PROGRESS"
      fail=$((fail + 1))
    else
      echo "OK -> $dest" >> "$LOG"
      ok=$((ok + 1))
    fi
  fi
  echo "progress n=$n total=$total ok=$ok fail=$fail skip=$skip" > "$STATUS"
done < "$COMMANDS"

echo "DONE total=$total ok=$ok fail=$fail skip=$skip finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$LOG" >> "$PROGRESS"
echo "running=0 ok=$ok fail=$fail skip=$skip finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STATUS"
