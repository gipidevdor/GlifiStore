#!/usr/bin/env bash
# Perl SDK client benchmark suite (sequential + concurrent + mixed-owner batch) against glifistored.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date -u +%Y%m%d-%H%M%S)"
sdk_version="$(tr -d '[:space:]' <"$root/VERSION")"
outdir="${1:-$root/benchmark-results-perl-${sdk_version}-${stamp}}"
daemon="${GLIFISTORED:-}"
perl="${PERL:-perl}"
host="127.0.0.1"
ops="${OPS:-100000}"
warmup="${WARMUP:-1}"
repeats="${REPEATS:-7}"
worker_hash_seed="${WORKER_HASH_SEED:-}"

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

mkdir -p "$outdir/perl" "$outdir/logs"
export PERL5LIB="$root/sdk/perl/lib${PERL5LIB:+:$PERL5LIB}"

{
  echo "captured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git -C "$root" rev-parse HEAD)"
  echo "perl=$($perl -V:version -V:archname | tr '\n' ' ')"
  echo "perl_sdk_version=$($perl -MGlifiStore -e 'print \$GlifiStore::VERSION')"
  echo "glifistored=$daemon"
  echo "ops=$ops warmup=$warmup repeats=$repeats"
  echo "workload=ordered PUT/GET pipeline read-after-write, value_size=64"
  echo "storage_mode=volatile"
  echo "worker_hash_seed=${worker_hash_seed:-default-fnv}"
  echo "note=publishes sequential, execute_worker_pipelines, and execute_batch modes"
} >"$outdir/environment.txt"

start_server() {
  local workers="$1" port_file="$2" log_file="$3"
  local routing_args=()
  if [[ -n "$worker_hash_seed" ]]; then
    routing_args=(--worker-hash-seed "$worker_hash_seed")
  fi
  "$daemon" --bind "$host" --port 0 --workers "$workers" \
    --storage-mode volatile --executor-affinity "${routing_args[@]}" --quiet \
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
  echo "# Perl SDK client benchmark"
  echo
  echo "- SDK version: \`$sdk_version\`"
  echo "- Ops (PUT/GET pairs): \`$ops\`"
  echo "- Warmup/repeats: \`$warmup\` / \`$repeats\`"
  echo "- Modes: sequential drain + concurrent \`execute_worker_pipelines\` + mixed-owner \`execute_batch\`"
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
    echo "running perl sequential $label"
    "$perl" "$root/sdk/perl/benchmarks/client_benchmark.pl" \
      --host "$host" --port "$port" --workers "$w" --ops "$ops" \
      --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
      --no-concurrent \
      | tee "$outdir/perl/sequential-${label}.txt"

    if [[ "$w" -gt 1 ]]; then
      echo "running perl concurrent $label"
      "$perl" "$root/sdk/perl/benchmarks/client_benchmark.pl" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --concurrent \
        | tee "$outdir/perl/concurrent-${label}.txt"

      echo "running perl mixed-owner batch $label"
      "$perl" "$root/sdk/perl/benchmarks/client_benchmark.pl" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --batch \
        | tee "$outdir/perl/batch-${label}.txt"
    fi
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
for path in sorted(outdir.glob("perl/*.txt")):
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

# Contrast concurrent vs sequential at same workers/pipeline.
seq = {(r["workers"], r["pipeline_pairs"]): r for r in rows if "sequential" in r["execution"]}
conc = {(r["workers"], r["pipeline_pairs"]): r for r in rows if "concurrent" in r["execution"]}

lines = [
    f"# GlifiStore Perl client benchmarks — version `{sdk_version}`",
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
    "## Concurrent vs sequential (same Workers / pipeline)",
    "",
    "| Workers | Pipeline | Sequential ops/s | Concurrent ops/s | Gain |",
    "| ---: | ---: | ---: | ---: | ---: |",
])
for key in sorted(conc):
    s = seq.get(key)
    c = conc[key]
    if not s:
        continue
    gain = c["median_ops_per_second"] / s["median_ops_per_second"] if s["median_ops_per_second"] else 0
    lines.append(
        f"| {key[0]} | {key[1]} | {s['median_ops_per_second']:,.0f} | "
        f"{c['median_ops_per_second']:,.0f} | {gain:.2f}× |"
    )

lines.extend([
    "",
    "## Notes",
    "",
    "- `concurrent` uses `execute_worker_pipelines` (one select loop, overlap across Workers).",
    "- `batch` interleaves owners, groups once, overlaps Workers, and restores caller order.",
    "- `sequential` drains Workers one after another (fair compare to Python sequential).",
    "- Workers=1 has no concurrent mode (single connection).",
    "- Same-host loopback; do not treat as production capacity.",
    "- Product scale still comes from one client per prefork process, not ithreads.",
    "",
])
(outdir / "summary.md").write_text("\n".join(lines), encoding="utf-8")
(outdir / "README.md").write_text(
    f"""# GlifiStore Perl SDK benchmarks — {sdk_version}

Sequential, concurrent (`execute_worker_pipelines`), and mixed-owner (`execute_batch`) matrix for
the pure-Perl client.

## Reproduce

```bash
./scripts/benchmark_perl_client.sh
```

Optional: `OPS`, `WARMUP`, `REPEATS`, `WORKER_HASH_SEED`, `GLIFISTORED`, `PERL`.
""",
    encoding="utf-8",
)
print(f"wrote {outdir / 'summary.md'}")
PY

trap - EXIT
echo "Perl SDK benchmarks published under $outdir"
