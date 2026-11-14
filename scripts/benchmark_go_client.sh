#!/usr/bin/env bash
# Go SDK client benchmark suite against an external glifistored.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date -u +%Y%m%d-%H%M%S)"
sdk_version="$(tr -d '[:space:]' <"$root/VERSION")"
outdir="${1:-$root/benchmark-results-go-${sdk_version}-${stamp}}"
daemon="${GLIFISTORED:-}"
go_bin="${GO:-go}"
host="127.0.0.1"
ops="${OPS:-100000}"
warmup="${WARMUP:-1}"
repeats="${REPEATS:-7}"

prefer_bins=(
  "$root/build/macos-native-release"
  "$root/build/macos-release"
  "$root/build/macos-debug"
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
if [[ -z "$daemon" || ! -x "$daemon" ]]; then
  echo "missing glifistored; build a release/debug preset first" >&2
  exit 1
fi

mkdir -p "$outdir/go" "$outdir/logs"
mkdir -p "$root/sdk/go/bin"
(cd "$root/sdk/go" && "$go_bin" build -o bin/glifistore-bench ./cmd/glifistore-bench)
bench="$root/sdk/go/bin/glifistore-bench"

{
  echo "captured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git -C "$root" rev-parse HEAD)"
  echo "go=$($go_bin version)"
  echo "go_sdk_version=$sdk_version"
  echo "glifistored=$daemon"
  echo "ops=$ops warmup=$warmup repeats=$repeats"
  echo "workload=ordered PUT/GET pipeline read-after-write, value_size=64"
  echo "storage_mode=volatile"
} >"$outdir/environment.txt"

start_server() {
  local workers="$1" port_file="$2" log_file="$3"
  "$daemon" --bind "$host" --port 0 --workers "$workers" \
    --storage-mode volatile --executor-affinity --quiet \
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

workers=(1 2 4)
pipelines=(1 8 32 128)
{
  echo "# Go SDK client benchmark"
  echo
  echo "- SDK version: \`$sdk_version\`"
  echo "- Ops (PUT/GET pairs): \`$ops\`"
  echo "- Warmup/repeats: \`$warmup\` / \`$repeats\`"
  echo
} >"$outdir/commands.md"

for w in "${workers[@]}"; do
  port_file="$outdir/logs/server-w${w}.port"
  log_file="$outdir/logs/server-w${w}.log"
  start_server "$w" "$port_file" "$log_file"
  port="$(cat "$port_file")"
  echo "- workers=$w listen=$host:$port" >>"$outdir/commands.md"

  for p in "${pipelines[@]}"; do
    label="w${w}-p${p}"
    echo "running go concurrent $label"
    "$bench" --host "$host" --port "$port" --workers "$w" --ops "$ops" \
      --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution concurrent \
      | tee "$outdir/go/concurrent-${label}.txt"

    echo "running go sequential $label"
    "$bench" --host "$host" --port "$port" --workers "$w" --ops "$ops" \
      --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution sequential \
      | tee "$outdir/go/sequential-${label}.txt"
  done
  stop_server "$port_file"
done

python3 - "$outdir" "$sdk_version" <<'PY'
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

outdir = Path(sys.argv[1])
sdk_version = sys.argv[2]
pattern = re.compile(
    r"name=(?P<name>\S+)\s+"
    r"sdk_version=(?P<sdk_version>\S+)\s+"
    r"runtime=(?P<runtime>\S+)\s+"
    r"execution=(?P<execution>\S+)\s+"
    r"workers=(?P<workers>\d+)\s+"
    r"pipeline_pairs=(?P<pipeline_pairs>\d+)\s+"
    r"operations=(?P<operations>\d+)\s+"
    r"samples=(?P<samples>\d+)\s+"
    r"median_seconds=(?P<median_seconds>[0-9.eE+-]+)\s+"
    r"min_seconds=(?P<min_seconds>[0-9.eE+-]+)\s+"
    r"max_seconds=(?P<max_seconds>[0-9.eE+-]+)\s+"
    r"median_ops_per_second=(?P<median_ops_per_second>[0-9.eE+-]+)\s+"
    r"min_ops_per_second=(?P<min_ops_per_second>[0-9.eE+-]+)\s+"
    r"max_ops_per_second=(?P<max_ops_per_second>[0-9.eE+-]+)"
)
rows = []
for path in sorted(outdir.glob("go/*.txt")):
    match = pattern.search(path.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(f"could not parse {path}")
    row = match.groupdict()
    row["file"] = str(path.relative_to(outdir))
    for key in ("workers", "pipeline_pairs", "operations", "samples"):
        row[key] = int(row[key])
    for key in (
        "median_seconds", "min_seconds", "max_seconds",
        "median_ops_per_second", "min_ops_per_second", "max_ops_per_second",
    ):
        row[key] = float(row[key])
    rows.append(row)

(outdir / "results.json").write_text(
    json.dumps({"sdk_version": sdk_version, "results": rows}, indent=2) + "\n",
    encoding="utf-8",
)
lines = [
    f"# GlifiStore Go client benchmarks — version `{sdk_version}`",
    "",
    f"Parsed `{len(rows)}` result files from `{outdir.name}`.",
    "",
    "Workload: validated ordered `PUT`/`GET` pipeline read-after-write, value size 64 bytes,",
    "volatile `glifistored`, same-host loopback. Median ops/s is the comparison statistic.",
    "",
    "| Execution | Workers | Pipeline pairs | Median ops/s | Min ops/s | Max ops/s | Median s |",
    "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
]
for row in rows:
    lines.append(
        "| {execution} | {workers} | {pipeline_pairs} | {median_ops_per_second:,.0f} | "
        "{min_ops_per_second:,.0f} | {max_ops_per_second:,.0f} | {median_seconds:.6f} |".format(**row)
    )
lines.extend([
    "",
    "## Notes",
    "",
    "- `concurrent` uses one goroutine per Worker against one shared `Client`.",
    "- `sequential` drains Workers one after another.",
    "- Same-host loopback; do not treat as production capacity.",
    "",
])
(outdir / "summary.md").write_text("\n".join(lines), encoding="utf-8")
print(f"wrote {outdir / 'summary.md'}")
PY

trap - EXIT
echo "Go SDK benchmarks published under $outdir"
