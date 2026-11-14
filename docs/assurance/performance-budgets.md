# Performance and soak budgets

**Normative** · hosted CI regression, hardware gates, soak and overload · reviewed 2026-08-27

The budget classes that link performance, soak and overload evidence to quality gates, and the
absolute thresholds still waiting for hardware. The page is normative for budget classes and
descriptive for advisory snapshots.

Authority: [`engineering/performance/budgets.yaml`](../../engineering/performance/budgets.yaml).
Validator: `python3 engineering/tools/validate_perf_budgets.py`.

The claim ceiling stays architectural prototype. Hosted GitHub runners produce regression signals
only. An absolute throughput or p99 product claim requires the labeled hardware runner
`glifistore-linux-perf` and a `pass-candidate` manifest: label `hardware` in the
[evidence taxonomy](evidence-taxonomy.md).

## Budget classes

| ID | Gate | Enforcement |
| --- | --- | --- |
| `HOSTED-MEDIAN-REGRESSION` | `GATE-PERFORMANCE` | Advisory hosted-CI signal; 10% median review threshold only for environment-compatible, disjoint ranges (`GS-PERF-REGRESSION-001`) |
| `HARDWARE-PAIRED-LINUX-AB` | `GATE-PERFORMANCE` | Self-hosted Linux hard-pin A/B (`GS-PERF-BUDGET-001`); `specified_waiting_for_runner` |
| `HARDWARE-SCALE-EFFICIENCY-1-8` | `GATE-PERFORMANCE` | Absolute placeholder: ≥80% efficiency 1→8 pairs vs physical cores (or named bottleneck); `specified_waiting_for_runner` / evidence `hardware` |
| `HARDWARE-ABSOLUTE-P99-GET` | `GATE-PERFORMANCE` | Absolute placeholder: p99 GET latency (ns TBD from runner); `specified_waiting_for_runner` / evidence `hardware` |
| `HARDWARE-ABSOLUTE-P99-PUT` | `GATE-PERFORMANCE` | Absolute placeholder: p99 PUT latency (ns TBD from runner); `specified_waiting_for_runner` / evidence `hardware` |
| `ADVISORY-EMBEDDED-DURABLE-GET-P99` | `GATE-PERFORMANCE` | Advisory macOS snapshot only — not a release gate (`local`) |
| `SOAK-SOFTWARE-SMOKE` | `GATE-SOAK` | PR/push smoke (`GS-OPS-SOAK-001`) |
| `SOAK-SOFTWARE-EXTENDED` | `GATE-SOAK` | Scheduled/manual long/1h/4h software soak |
| `OVERLOAD-RUNBOOK` | `GATE-OPS-RUNBOOKS` | Drain/overload runbook exercised in ops-runbooks CI |

## Absolute placeholders (not enforced)

These placeholders are scaffolding. Each stays `specified_waiting_for_runner` until retained
hardware evidence exists, and its numeric threshold may come only from a `glifistore-linux-perf`
`pass-candidate` artifact. This page does not claim `ACCETTATA_PER_RILASCIO`, E3 or E4.

| Placeholder | Intended metric | Draft rule (not enforced) | Evidence class | Status |
| --- | --- | --- | --- | --- |
| Scale 1/2/4/8 | efficiency vs 1-pair median ops/s on same pin | ≥ 80% to physical-core count, or documented bottleneck removal | `hardware` | `specified_waiting_for_runner` |
| No CPU migration | `perf` `cpu-migrations` per cell | == 0 on hard-pinned run (harness flags otherwise) | `hardware` | `specified_waiting_for_runner` |
| Client/server split | disjoint `SERVER_CPUS` / `CLIENT_CPUS` | required for pass-candidate | `hardware` | `specified_waiting_for_runner` |
| Absolute p99 GET | GET p99 latency (ns) | TBD — do not invent | `hardware` | `specified_waiting_for_runner` |
| Absolute p99 PUT | PUT p99 latency (ns) | TBD — do not invent | `hardware` | `specified_waiting_for_runner` |
| Interleaved A/B | old/new/new/old | hard-reject >5% affine PUT 2t regression when closing ADR 0036 V12 | `hardware` | `specified_waiting_for_runner` |

Harness: [`scripts/benchmark_paired_linux_ab.sh`](../../scripts/benchmark_paired_linux_ab.sh).
Methodology: [paired shards Linux P1 harness](../benchmarks/paired-shards-linux-p1.md).

## Open blockers

1. A self-hosted runner labeled `glifistore-linux-perf` that publishes a retained `pass-candidate`
   manifest (hard pins, NUMA, disjoint client/server CPUs, migration counters).
2. A physical E3 durability lab, separate from this A/B harness. E3/E4 remain open; rehearsal is
   not certification ([evidence taxonomy](evidence-taxonomy.md)).
3. This scaffolding alone promotes nothing to `ACCETTATA_PER_RILASCIO`.

## Related

- [Benchmark standard](../spec/benchmark-standard.md)
- [Paired shards Linux P1 harness](../benchmarks/paired-shards-linux-p1.md)
- [Evidence taxonomy](evidence-taxonomy.md)
- [Soak profiles](../operations/soak.md)
- [Graceful drain and overload](../operations/graceful-drain-and-overload.md)
