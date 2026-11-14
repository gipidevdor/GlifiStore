# Paired shards Linux hard-pinned A/B harness

**Descriptive** · Linux hard-pinned A/B harness for the P1 gate · reviewed 2026-07-31

## Gate status

**Harness ready; gate not closed.** Hosted GitHub `ubuntu-24.04` runners and any macOS advisory
affinity remain inconclusive for this P1. Closure needs an isolated Linux machine with hard CPU
pins, NUMA binding, `perf`, disjoint client/server CPU lists, zero unexpected `cpu-migrations`, and
a working set above LLC.

Evidence class: `hardware` only after runner `glifistore-linux-perf` publishes
`status=pass-candidate`. Absolute budget placeholders remain
`specified_waiting_for_runner` — see
[`performance-budgets.md`](../assurance/performance-budgets.md) and
[`evidence-taxonomy.md`](../assurance/evidence-taxonomy.md).

The gate is blocked on:

1. `glifistore-linux-perf` self-hosted runner + retained pass-candidate;
2. Physical E3 durability lab (separate campaign; not this A/B script).

Do not invent runner results. Do not claim `ACCETTATA_PER_RILASCIO`, E3, or E4 from this harness alone.

## Why macOS cannot close the gate

- Mach affinity is advisory (`executor_affinity_semantics=mach-advisory`).
- No `taskset` / `numactl` / `perf` TID separation equivalent to the methodology in
  [`paired-shards-plan.md`](paired-shards-plan.md).
- Prior multi-pair and PUT→GET cells left inconclusive for that reason (compact base ~−3% volatile,
  durable multi-pair scale, PUT→GET 1-pair).

## Harness

```bash
# Linux self-hosted runner or dedicated machine
SERVER_CPUS=0-7 CLIENT_CPUS=8-15 NUMA_NODE=0 \
OLD_BIN=/path/to/baseline/glifistore_server_benchmarks \
NEW_BIN=/path/to/candidate/glifistore_server_benchmarks \
PAIRS='1 2 4 8' \
CHECK_CPU_MIGRATION=1 \
LATENCY_SAMPLE_STRIDE=1 \
LATENCY_SPLIT=1 \
./scripts/benchmark_paired_linux_ab.sh

# macOS smoke (explicitly inconclusive / evidence_class=local)
REQUIRE_LINUX_TOOLS=0 PAIRS='1 2' PIPELINES='1 8' REPEATS=1 WARMUP=0 \
  ./scripts/benchmark_paired_linux_ab.sh
```

Affinity helpers: `scripts/lib/linux_cpu_lists.sh` (expand / disjoint / subset). The in-process
benchmark binary is taskset onto `SERVER_CPUS`; `CLIENT_CPUS` must be disjoint and left free of
server load (documented combined-load rule until a split-process client exists).

Optional latency knobs (candidate binary; older `OLD_BIN` probed via `--help`):

| Env / flag | Meaning |
| --- | --- |
| `LATENCY_SAMPLE_STRIDE` / `--latency-sample-stride N` | Keep every Nth latency sample (default 1 = densest) |
| `LATENCY_SPLIT` / `--latency-split` | Separate GET vs PUT p50/p95/p99/p99.9/max lines |

Artifacts land in:

```text
benchmark-results/paired-shards/<commit>/<platform>/<run-id>/
  environment.txt
  topology/          # affinity notes, expanded CPU lists
  numa/
  perf/              # perf stat + *.migration.txt
  samples/
  scaling/efficiency-scaffold.csv   # placeholders only until real medians
  command-lines.txt
  report.md
  manifest.txt       # status + evidence_class + wave6_blockers
```

`platform` is `linux` or `macos-advisory`. Any missing pin, client/server overlap, migration
counter, or missing tool forces `inconclusive-*` / `incomplete-*` in `manifest.txt` — never a
silent pass. Interleave order when `OLD_BIN != NEW_BIN`: **old-a / new-a / new-b / old-b**.

## Minimum matrix for gate close

| Axis | Values |
|---|---|
| pairs | 1, 2, 4, 8 (capped to server pin / 2 for Reader+Writer) |
| mix | GET; PUT→GET p1; 99/1 |
| values | 64 B (extend 1 KiB–256 KiB after first pass) |
| pipeline | 1, 8, 32 |
| durability | volatile first; durable-periodic for multi-pair follow-up |
| topology | disjoint client/server CPUs; no SMT sibling sharing in baseline |
| migration | `perf` `cpu-migrations` == 0 when `CHECK_CPU_MIGRATION=1` |
| working set | declare LLC size; include a >LLC cell |
| scale | ≥80% efficiency vs 1-pair **or** named bottleneck (placeholder until runner) |

Order: interleaved `old/new/new/old`. Invalid if responses, request ids, or final verify fail.

## CI

`.github/workflows/paired-linux-performance.yml` is `workflow_dispatch` only and skips unless the
runner label `glifistore-linux-perf` is present (self-hosted). Hosted Ubuntu jobs must not claim
this gate.

## Residual

Until a labeled Linux runner publishes `status=pass-candidate` with the evidence above, the P1
items that depended on hard-pinned A/B stay open. macOS samples under this harness are advisory
(`local`). Absolute p99 GET/PUT thresholds in
[`performance-budgets.md`](../assurance/performance-budgets.md) remain TBD.
