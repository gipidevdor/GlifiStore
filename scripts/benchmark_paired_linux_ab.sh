#!/usr/bin/env bash
# Paired-shards Linux hard-pinned A/B orchestrator (Phase 2 P1 / Wave 6 scaffold).
#
# Prepares the 1/2/4/8 pair matrix, interleaved old/new/new/old order, client/server
# core-split checks, and CPU-migration counter collection. Does NOT invent hardware
# proof: missing runner pins or tools force inconclusive-* / incomplete-* status.
#
# Absolute budgets and ACCETTATA_PER_RILASCIO remain blocked on runner label
# glifistore-linux-perf plus a retained pass-candidate manifest (evidence class:
# hardware). Physical E3 lab is a separate Wave 6 durability blocker.
#
# macOS advisory runs are allowed for harness smoke only and MUST be marked
# inconclusive; they do not close the gate.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/linux_cpu_lists.sh
source "$root/scripts/lib/linux_cpu_lists.sh"

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
commit="$(git -C "$root" rev-parse --short HEAD)"
dirty=""
if ! git -C "$root" diff --quiet || ! git -C "$root" diff --cached --quiet; then
  dirty="-dirty"
fi

platform="linux"
evidence_class="hardware"
if [[ "$(uname -s)" == "Darwin" ]]; then
  platform="macos-advisory"
  evidence_class="local"
fi

run_id="${RUN_ID:-${stamp}-paired-linux-ab}"
output="${OUTPUT_DIR:-$root/benchmark-results/paired-shards/${commit}${dirty}/${platform}/${run_id}}"
preset="${BENCHMARK_PRESET:-}"
if [[ -z "$preset" ]]; then
  if [[ "$(uname -s)" == "Darwin" ]]; then
    preset="macos-release"
  else
    preset="unix-release"
  fi
fi
cmake_bin="${CMAKE:-cmake}"
if [[ -x /Library/Perl/5.34/darwin-thread-multi-2level/auto/share/dist/Alien-cmake3/bin/cmake ]]; then
  cmake_bin="${CMAKE:-/Library/Perl/5.34/darwin-thread-multi-2level/auto/share/dist/Alien-cmake3/bin/cmake}"
fi

old_bin="${OLD_BIN:-}"
new_bin="${NEW_BIN:-}"
pairs_list="${PAIRS:-1 2 4 8}"
pipelines="${PIPELINES:-1 8 32}"
value_sizes="${VALUE_SIZES:-64}"
workloads="${WORKLOADS:-get-only read-after-write read-99-write-1}"
storage_modes="${STORAGE_MODES:-volatile}"
ops="${BENCHMARK_OPS:-100000}"
warmup="${BENCHMARK_WARMUP:-1}"
repeats="${BENCHMARK_REPEATS:-7}"
clients_per_pair="${CLIENTS_PER_PAIR:-1}"
require_linux_tools="${REQUIRE_LINUX_TOOLS:-1}"
server_cpu_list="${SERVER_CPUS:-}"
client_cpu_list="${CLIENT_CPUS:-}"
numa_node="${NUMA_NODE:-}"
check_migration="${CHECK_CPU_MIGRATION:-1}"
min_scale_efficiency_pct="${MIN_SCALE_EFFICIENCY_PCT:-80}"
latency_sample_stride="${LATENCY_SAMPLE_STRIDE:-1}"
latency_split="${LATENCY_SPLIT:-1}"
wave6_blockers="glifistore-linux-perf;physical-E3-lab"

mkdir -p "$output"/{samples,perf,numa,topology,scaling}
status="pass-candidate"
notes=()
migration_failures=0

if [[ "$(uname -s)" != "Linux" ]]; then
  status="inconclusive-not-linux-hard-pinned"
  notes+=("host=$(uname -s): hard pin/perf/NUMA gate cannot close here")
  notes+=("evidence_class=$evidence_class (Wave 6 scaling claims require hardware)")
  require_linux_tools=0
fi

need() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    if [[ "$require_linux_tools" == "1" ]]; then
      echo "missing required tool: $cmd" >&2
      exit 2
    fi
    notes+=("missing-tool=$cmd")
    status="inconclusive-missing-tools"
    return 1
  fi
  return 0
}

need_linux() {
  [[ "$(uname -s)" == "Linux" ]] || return 1
  need "$1"
}

mark_status() {
  local next="$1"
  case "$status" in
    pass-candidate) status="$next" ;;
    *) ;;
  esac
}

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git -C "$root" rev-parse HEAD)"
  echo "git_describe=$(git -C "$root" describe --always --dirty)"
  echo "evidence_class=$evidence_class"
  echo "wave6_scaffold=1"
  echo "wave6_blockers=$wave6_blockers"
  echo "absolute_budgets=specified_waiting_for_runner"
  echo "preset=$preset"
  echo "pairs=$pairs_list"
  echo "pipelines=$pipelines"
  echo "value_sizes=$value_sizes"
  echo "workloads=$workloads"
  echo "storage_modes=$storage_modes"
  echo "ops=$ops"
  echo "warmup=$warmup"
  echo "repeats=$repeats"
  echo "clients_per_pair=$clients_per_pair"
  echo "server_cpus=${server_cpu_list:-unset}"
  echo "client_cpus=${client_cpu_list:-unset}"
  echo "numa_node=${numa_node:-unset}"
  echo "check_cpu_migration=$check_migration"
  echo "min_scale_efficiency_pct=$min_scale_efficiency_pct"
  echo "latency_sample_stride=$latency_sample_stride"
  echo "latency_split=$latency_split"
  echo "interleave_order=old-a/new-a/new-b/old-b"
  echo "old_bin=${old_bin:-build-in-tree}"
  echo "new_bin=${new_bin:-build-in-tree}"
  uname -a
  command -v lscpu >/dev/null && lscpu || true
  sysctl -n machdep.cpu.brand_string 2>/dev/null || true
} > "$output/environment.txt"

if need_linux lscpu; then
  lscpu -e > "$output/topology/lscpu-e.txt" || true
  lscpu > "$output/topology/lscpu.txt" || true
fi
if need_linux numactl; then
  numactl --hardware > "$output/numa/hardware.txt" || true
fi

: > "$output/topology/affinity-notes.txt"
if [[ -n "$server_cpu_list" ]]; then
  if ! server_cpu_count="$(count_cpu_list "$server_cpu_list")"; then
    notes+=("malformed-SERVER_CPUS=$server_cpu_list")
    mark_status "inconclusive-malformed-server-cpus"
    server_cpu_count=0
  else
    echo "server_cpu_count=$server_cpu_count" >> "$output/topology/affinity-notes.txt"
    expand_cpu_list "$server_cpu_list" > "$output/topology/server-cpus.txt"
  fi
else
  server_cpu_count=0
  echo "server_cpu_list=unset" >> "$output/topology/affinity-notes.txt"
  if [[ "$(uname -s)" == "Linux" ]]; then
    mark_status "inconclusive-no-server-cpu-set"
    notes+=("SERVER_CPUS unset; hard pin required for Wave 6")
  fi
fi

if [[ -n "$client_cpu_list" ]]; then
  if ! client_cpu_count="$(count_cpu_list "$client_cpu_list")"; then
    notes+=("malformed-CLIENT_CPUS=$client_cpu_list")
    mark_status "inconclusive-malformed-client-cpus"
    client_cpu_count=0
  else
    echo "client_cpu_count=$client_cpu_count" >> "$output/topology/affinity-notes.txt"
    expand_cpu_list "$client_cpu_list" > "$output/topology/client-cpus.txt"
  fi
  if [[ -n "$server_cpu_list" && "$server_cpu_count" -gt 0 && "$client_cpu_count" -gt 0 ]]; then
    if cpu_lists_disjoint "$server_cpu_list" "$client_cpu_list"; then
      echo "client_server_disjoint=yes" >> "$output/topology/affinity-notes.txt"
    else
      echo "client_server_disjoint=no" >> "$output/topology/affinity-notes.txt"
      notes+=("SERVER_CPUS and CLIENT_CPUS overlap")
      mark_status "inconclusive-client-server-cpu-overlap"
    fi
  fi
  echo "client_cpu_list=$client_cpu_list (reserved / load-generator cores; must stay disjoint from SERVER_CPUS)" \
    >> "$output/topology/affinity-notes.txt"
  echo "note=glifistore_server_benchmarks is in-process; CLIENT_CPUS documents reserved cores left free of server taskset" \
    >> "$output/topology/affinity-notes.txt"
else
  client_cpu_count=0
  echo "client_cpu_list=unset; mark combined-load if shared host" \
    >> "$output/topology/affinity-notes.txt"
  if [[ "$(uname -s)" == "Linux" ]]; then
    mark_status "inconclusive-no-client-cpu-set"
    notes+=("CLIENT_CPUS unset; client/server core split required for Wave 6")
  fi
fi

capped_pairs=()
for pairs in $pairs_list; do
  if [[ "$server_cpu_count" -gt 0 ]]; then
    needed=$((pairs * 2))
    if (( needed > server_cpu_count )); then
      notes+=("pairs=$pairs capped: need ${needed} server CPUs for Reader+Writer, have $server_cpu_count")
      mark_status "incomplete-pairs-exceed-server-cpus"
      continue
    fi
  fi
  capped_pairs+=("$pairs")
done
if ((${#capped_pairs[@]} == 0)); then
  for pairs in $pairs_list; do
    capped_pairs+=("$pairs")
  done
fi
echo "pairs_effective=${capped_pairs[*]}" >> "$output/environment.txt"

if [[ -z "$new_bin" ]]; then
  "$cmake_bin" --preset "$preset"
  "$cmake_bin" --build --preset "$preset" --target glifistore_server_benchmarks
  new_bin="$root/build/$preset/glifistore_server_benchmarks"
fi
if [[ -z "$old_bin" ]]; then
  old_bin="$new_bin"
  notes+=("single-binary-mode=no-baseline-OLD_BIN; scale matrix only")
  mark_status "incomplete-no-old-binary"
fi

supports_latency_knobs() {
  local bin="$1"
  "$bin" --help 2>&1 | grep -q -- '--latency-sample-stride'
}

new_latency_knobs=0
old_latency_knobs=0
if supports_latency_knobs "$new_bin"; then
  new_latency_knobs=1
else
  notes+=("new_bin lacks --latency-sample-stride/--latency-split; rebuild candidate")
fi
if [[ "$old_bin" == "$new_bin" ]]; then
  old_latency_knobs="$new_latency_knobs"
elif supports_latency_knobs "$old_bin"; then
  old_latency_knobs=1
else
  notes+=("old_bin lacks latency knobs; A/B cells omit stride/split for old only")
fi
echo "new_latency_knobs=$new_latency_knobs" >> "$output/environment.txt"
echo "old_latency_knobs=$old_latency_knobs" >> "$output/environment.txt"

perf_events="task-clock,cycles,instructions,context-switches,cpu-migrations,page-faults"

record_migration_check() {
  local stem="$1"
  local stat_file="$output/perf/${stem}.stat.txt"
  local migrations=""
  if [[ ! -f "$stat_file" ]]; then
    echo "migrations=unavailable" > "$output/perf/${stem}.migration.txt"
    return 0
  fi
  migrations="$(
    awk '
      /cpu-migrations|CPU migrations|migrations/ {
        for (i = 1; i <= NF; i++) {
          gsub(/,/, "", $i)
          if ($i ~ /^[0-9]+$/) { print $i; exit }
        }
      }
    ' "$stat_file" 2>/dev/null || true
  )"
  if [[ -z "$migrations" ]]; then
    echo "migrations=unparsed" > "$output/perf/${stem}.migration.txt"
    notes+=("migration-counter-unparsed:$stem")
    mark_status "inconclusive-migration-counter"
    return 0
  fi
  echo "migrations=$migrations" > "$output/perf/${stem}.migration.txt"
  if [[ "$check_migration" == "1" && "$migrations" != "0" ]]; then
    notes+=("cpu-migrations=$migrations cell=$stem")
    migration_failures=$((migration_failures + 1))
    mark_status "inconclusive-cpu-migrations"
  fi
}

run_cell() {
  local label="$1"
  local bin="$2"
  local pairs="$3"
  local pipeline="$4"
  local value_size="$5"
  local workload="$6"
  local mode="$7"
  local knobs_ok="$8"
  local clients=$((pairs * clients_per_pair))
  local stem="${label}-p${pairs}-c${clients}-pipe${pipeline}-v${value_size}-${workload}-${mode}"
  local args=(
    --ops "$ops"
    --workers "$pairs"
    --clients "$clients"
    --pipeline "$pipeline"
    --value-size "$value_size"
    --workload "$workload"
    --storage-mode "$mode"
    --warmup "$warmup"
    --repeats "$repeats"
    --latency
    --executor-affinity
  )
  if [[ "$knobs_ok" == "1" ]]; then
    args+=(--latency-sample-stride "$latency_sample_stride")
    if [[ "$latency_split" == "1" ]]; then
      args+=(--latency-split)
    fi
  fi
  echo "$stem ${args[*]}" >> "$output/command-lines.txt"
  if [[ "$(uname -s)" == "Linux" ]] && command -v perf >/dev/null 2>&1; then
    if [[ -n "$numa_node" ]] && command -v numactl >/dev/null 2>&1 && \
       [[ -n "$server_cpu_list" ]] && command -v taskset >/dev/null 2>&1; then
      perf stat -e "$perf_events" -o "$output/perf/${stem}.stat.txt" -- \
        numactl --cpunodebind="$numa_node" --membind="$numa_node" \
        taskset -c "$server_cpu_list" \
        "$bin" "${args[@]}" > "$output/samples/${stem}.txt"
    elif [[ -n "$server_cpu_list" ]] && command -v taskset >/dev/null 2>&1; then
      perf stat -e "$perf_events" -o "$output/perf/${stem}.stat.txt" -- \
        taskset -c "$server_cpu_list" \
        "$bin" "${args[@]}" > "$output/samples/${stem}.txt"
    else
      perf stat -e "$perf_events" -o "$output/perf/${stem}.stat.txt" -- \
        "$bin" "${args[@]}" > "$output/samples/${stem}.txt"
    fi
    record_migration_check "$stem"
  elif [[ -n "$server_cpu_list" ]] && command -v taskset >/dev/null 2>&1; then
    taskset -c "$server_cpu_list" "$bin" "${args[@]}" > "$output/samples/${stem}.txt"
  else
    "$bin" "${args[@]}" > "$output/samples/${stem}.txt"
  fi
}

for pairs in "${capped_pairs[@]}"; do
  for pipeline in $pipelines; do
    for value_size in $value_sizes; do
      for workload in $workloads; do
        for mode in $storage_modes; do
          if [[ "$old_bin" == "$new_bin" ]]; then
            run_cell "solo" "$new_bin" "$pairs" "$pipeline" "$value_size" "$workload" "$mode" "$new_latency_knobs"
          else
            run_cell "old-a" "$old_bin" "$pairs" "$pipeline" "$value_size" "$workload" "$mode" "$old_latency_knobs"
            run_cell "new-a" "$new_bin" "$pairs" "$pipeline" "$value_size" "$workload" "$mode" "$new_latency_knobs"
            run_cell "new-b" "$new_bin" "$pairs" "$pipeline" "$value_size" "$workload" "$mode" "$new_latency_knobs"
            run_cell "old-b" "$old_bin" "$pairs" "$pipeline" "$value_size" "$workload" "$mode" "$old_latency_knobs"
          fi
        done
      done
    done
  done
done

{
  echo "# Scaling scaffold — evidence_class=$evidence_class"
  echo "# Placeholder threshold min_scale_efficiency_pct=$min_scale_efficiency_pct"
  echo "# status=specified_waiting_for_runner until glifistore-linux-perf pass-candidate"
  echo "pairs,label,median_ops_per_second,efficiency_vs_1pair_pct,note"
  for pairs in "${capped_pairs[@]}"; do
    for label in solo old-a new-a new-b old-b; do
      sample_glob="$output/samples/${label}-p${pairs}-c$((pairs * clients_per_pair))-pipe*-v*-*-*.txt"
      # shellcheck disable=SC2086
      first_sample="$(ls $sample_glob 2>/dev/null | head -n 1 || true)"
      if [[ -z "$first_sample" ]]; then
        echo "$pairs,$label,,,waiting_for_samples"
        continue
      fi
      median="$(awk '
        {
          for (i = 1; i <= NF; i++) {
            if ($i ~ /^median_ops_per_second=/) {
              split($i, a, "="); print a[2]; exit
            }
          }
        }
      ' "$first_sample")"
      echo "$pairs,$label,${median:-},,parsed_sample_only_not_a_claim"
    done
  done
} > "$output/scaling/efficiency-scaffold.csv"

{
  echo "# Paired Linux hard-pinned A/B — ${run_id}"
  echo
  echo "## Status"
  echo
  echo "**${status}**"
  echo
  echo "Evidence class: \`${evidence_class}\` (see docs/assurance/evidence-taxonomy.md)."
  echo
  echo "Wave 6 blockers (honest open):"
  echo "- Runner label \`glifistore-linux-perf\` with retained \`pass-candidate\` manifest"
  echo "- Physical E3 durability lab (separate from this A/B harness)"
  echo "- Absolute budget placeholders remain \`specified_waiting_for_runner\` — not ACCETTATA"
  echo
  echo "Platform: ${platform}. Notes:"
  for n in "${notes[@]:-}"; do
    echo "- $n"
  done
  if [[ "$migration_failures" -gt 0 ]]; then
    echo "- cpu-migration cells flagged: $migration_failures"
  fi
  echo
  echo "Artifacts: \`$output\`"
  echo
  echo "This report is auto-generated by \`scripts/benchmark_paired_linux_ab.sh\`."
  echo "A gate close requires Linux hard pins, disjoint client/server CPUs, perf TID"
  echo "separation, NUMA placement, working set > LLC, interleaved old/new/new/old,"
  echo "and no unexpected cpu-migrations — see docs/benchmarks/paired-shards-linux-p1.md"
  echo "and docs/assurance/performance-budgets.md."
} > "$output/report.md"

{
  echo "status=$status"
  echo "output=$output"
  echo "evidence_class=$evidence_class"
  echo "wave6_blockers=$wave6_blockers"
  echo "absolute_budgets=specified_waiting_for_runner"
  echo "migration_failures=$migration_failures"
  echo "pairs_effective=${capped_pairs[*]}"
} | tee "$output/manifest.txt"

echo "$output"
