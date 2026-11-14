# GlifiStore complete local benchmark rerun — 2026-08-26

## Verdict

The rerun confirms the architectural direction but does not close the performance program.
Immutable paired reads are fast and owner-bound GET scales well. End-to-end mixed traffic is still
limited by mutation publication/completion, durable throughput remains dominated by the physical
commit contract, memory amplification remains high, and maintenance overlap is the clearest P0
regression signal.

These results are local laboratory evidence. They are not production-capacity claims, a portable
Linux/BSD baseline, or E3/E4 durability evidence.

## Measurement identity and completeness

- Source: dirty pre-commit tree based on `fae45473862818b46cdc6ce7bdad11dfc61594ab`.
- Branch: `codex/ci-green-sanitizer-link`.
- Host: Apple M4, 10 physical/logical cores, 16 GiB RAM, macOS 25.6.0, APFS SSD.
- Build: `macos-release`, AppleClang 21, LTO off, native CPU tuning off.
- Placement: client and server share the local host/process; macOS affinity is advisory.
- Sampling: normally one warm-up plus seven measurements; durable/paired cases retain their
  recorded five- or seven-sample contract.
- Campaign: **219/219 commands completed successfully**.
- Strict normalized report: **165 suites / 180 results** in `results.json` and `results.md`.
- Dedicated evidence: 36 paired files, 6 compaction series and 12 maintenance series.
- Command contract: SHA-256
  `7c1218df64c20701d3f1db2663a86e42e49401a624242f0c679797c4982166a7`.

Missing from this row: hard CPU pinning, NUMA, hardware counters, isolated client/server hosts,
thermal telemetry, real Linux filesystem/device rows, multi-day soak and physical power-loss tests.

## Core engine

Median throughput, one owner and uniform keys:

| Value | GET copy | PUT | PUT batch | Read-after-write |
| ---: | ---: | ---: | ---: | ---: |
| 16 B | 4.570 M/s | 487.8 k/s | 679.6 k/s | 911.7 k/s |
| 64 B | 3.555 M/s | 478.1 k/s | 697.6 k/s | 914.1 k/s |
| 256 B | 2.999 M/s | 489.6 k/s | 661.5 k/s | 889.2 k/s |
| 1 KiB | 1.827 M/s | 429.8 k/s | 573.1 k/s | 709.4 k/s |
| 4 KiB | 795.0 k/s | 297.9 k/s | 393.8 k/s | 434.6 k/s |
| 64 KiB | 68.4 k/s | 58.2 k/s | 61.9 k/s | 61.0 k/s |
| 256 KiB | 16.7 k/s | 14.7 k/s | 14.8 k/s | 15.8 k/s |

The Index reaches 13.95 M hit lookups/s and 14.37 M miss lookups/s at 64-byte values. The complete
owning Store GET reaches 3.55 M/s, so the main remaining read cost is above the raw Index probe:
lease/admission, generation traversal and the owning value result matter more than another isolated
hash-table optimization.

The 64-byte/200k-key GET run reports roughly 230 MiB process RSS for about 12.2 MiB of logical value
bytes, approximately 19x. This is not a component allocation census, but it confirms that compact
metadata, fixed Segment cost and generation/delta lifetime need explicit byte accounting before the
memory concern can be closed.

## Embedded paired scaling

| Pairs | Owner GET | Uniform GET | Zipf GET | Owner PUT | Owner read-after-write |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.747 M/s | 3.698 M/s | 3.806 M/s | 582.4 k/s | 1.054 M/s |
| 2 | 9.190 M/s | 5.842 M/s | 6.489 M/s | 724.6 k/s | 1.256 M/s |
| 4 | 14.392 M/s | 10.885 M/s | 10.497 M/s | 1.191 M/s | 2.140 M/s |
| 8 | 24.504 M/s | 12.258 M/s | 10.872 M/s | 2.186 M/s | 3.323 M/s |

Owner GET reaches 3.84x at four pairs and 6.54x at eight pairs relative to one pair. The eight-pair
point is a stress observation on a ten-core machine once Reader and Writer activity is counted, not
the preferred deployment baseline. Uniform and Zipf results remain below owner-bound scaling,
confirming that locality and ownership-aware client placement are material.

## Immutable read primitive

At 64-byte values, the isolated paired span path reaches 13.19 M GET/s with 125 ns median p99,
versus 5.96 M/s and 584 ns for the current owning Store result. At 95% GET / 5% PUT, it reaches
11.13 M/s and 167 ns p99, versus 3.95 M/s and 292 ns.

This proves the value of immutable publication and borrowed internal lifetimes. It does not justify
changing the public owning API or claiming the same advantage through TCP, where framing, kernel
copies and bounded output ownership remain mandatory.

The paired reactor A/B also identifies the mutation limit. At one pair / pipeline 32:

| Writes | Current reactor | Paired prototype | Paired difference |
| ---: | ---: | ---: | ---: |
| 0% | 766 k/s | 777 k/s | +1.5% |
| 1% | 672 k/s | 683 k/s | +1.6% |
| 5% | 510 k/s | 460 k/s | -9.7% |
| 10% | 493 k/s | 308 k/s | -37.6% |

The 1% case publishes one record per measured publication. Increasing write pressure therefore
amplifies fixed generation/publication work instead of reliably forming useful batches. Publication
deadline/pressure policy remains higher priority than replacing the read Index.

## TCP volatile profile

One pair, one connection, 64-byte values:

| Pipeline | Workload | Throughput | p50 | p99 | p99.9 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | GET | 45.6 k/s | 21.9 us | 50.2 us | 167.8 us |
| 8 | GET | 294.6 k/s | 26.0 us | 60.5 us | 155.3 us |
| 32 | GET | 885.9 k/s | 34.8 us | 41.6 us | 51.1 us |
| 128 | GET | 1.824 M/s | 65.9 us | 171.8 us | 267.9 us |
| 32 | GET 99% / PUT 1% | 747.5 k/s | 36.2 us | 87.1 us | 174.6 us |
| 32 | Read-after-write | 349.4 k/s | 112.5 us | 628.6 us | 2.58 ms |
| 128 | Read-after-write | 287.1 k/s | 392.0 us | 3.74 ms | 9.28 ms |

Pipeline 128 maximizes GET throughput but is not the balanced mixed-traffic profile. Pipeline 32
preserves substantially lower tails. At pipeline 32, GET throughput grows from 886 k/s at one pair
to 1.645 M/s at two, 2.593 M/s at four and 3.669 M/s at eight. Read-after-write does not scale the
same way: mutation queueing and completion dominate, and the eight-pair p99 reaches 5.01 ms.

## Durability on this APFS row

One pair, pipeline 32, 64-byte values:

| Mode | Workload | Throughput | p99 | p99.9 | Median commit |
| --- | --- | ---: | ---: | ---: | ---: |
| sync | GET | 35.2 k/s | 1.56 ms | 1.94 ms | n/a |
| sync | GET 99% / PUT 1% | 14.6 k/s | 6.84 ms | 7.06 ms | n/a |
| sync | Read-after-write | 385/s | 191.5 ms | 228.7 ms | n/a |
| group | GET | 45.4 k/s | 8.77 ms | 9.37 ms | 1.36 ms |
| group | GET 99% / PUT 1% | 14.5 k/s | 5.96 ms | 6.16 ms | 1.87 ms |
| group | Read-after-write | 395/s | 171.0 ms | 189.8 ms | 4.65 ms |
| periodic | GET | 44.7 k/s | 0.99 ms | 1.16 ms | 1.01 ms |
| periodic | GET 99% / PUT 1% | 31.3 k/s | 9.91 ms | 10.13 ms | 0.80 ms |
| periodic | Read-after-write | 11.9 k/s | 21.62 ms | 31.35 ms | 0.76 ms |

Periodic is not equivalent to sync/group: it can acknowledge before the next forced durable
boundary. Sync must not be optimized toward Mops/s under its current contract. Group batching is
useful only when it actually fills: the four-client sweep peaks at 12.4 k/s for a configured batch
of four. Configurations 16/32/128 still form median batches of only four and fall to roughly
1.1–1.5 k/s with p99 from 278 to 829 ms. Deadline/fairness and device-aware commit coordination
remain P0; platform defaults must not change from this APFS result alone.

## Compaction and maintenance

Compaction medians:

| Scenario | Copied | Compact time | Outcome |
| --- | ---: | ---: | --- |
| High reclaim | 31.0 MiB | 266 ms | 7/7 compacted and reopened |
| Medium reclaim | 127.0 MiB | 802 ms | 7/7 compacted and reopened |
| Low reclaim | 191.1 MiB | 1.162 s | 7/7 compacted and reopened |
| Copy heavy | 254.6 MiB | 1.495 s | 7/7 compacted and reopened |
| TTL 50% | 127.5 MiB | 550 ms | 7/7 compacted and reopened |
| No gain | 0 | 134 ms planning/verify | 0/7 rewrites, as required |

Maintenance overlap is the strongest negative result:

| Scenario | Disabled | Cooperative | Background |
| --- | ---: | ---: | ---: |
| Mixed throughput | 104.7 k/s | 45.2 k/s | 44.1 k/s |
| Mixed p99 | 479 us | 2.38 ms | 2.43 ms |
| Forced rotation | 112 ms | 372 ms | 365 ms |
| Publication wait in forced rotation | ~0 | 244 ms | 246 ms |
| Churn throughput | 4.68 k/s | 3.77 k/s | 3.59 k/s |
| Churn p99 | 3.66 ms | 4.38 ms | 5.03 ms |
| Idle process CPU duty | 0.0026% | 0.0028% | 0.0066% |

Idle cost remains negligible; overlap and publication coordination are the problem. The previous
same-host exploratory campaign reported about 191/221 ms forced rotation and 140/155 ms publication
wait for cooperative/background. The new 365–372 ms and 244–246 ms results are a serious regression
candidate, but not an accepted historical comparison: the prior environment record lacks the full
identity/contract fields now required by `benchmark_report.py`. It must be reproduced on an isolated
runner before attributing it to code.

## Exploratory comparison with the previous local campaign

The strict reporter correctly marks the old report environment as incompatible because it lacks
`runner_image`, `runner_image_version` and `benchmark_contract_sha256`. A manual same-host range
comparison matched all 180 standard results:

- 170 have overlapping min/max throughput intervals and are inconclusive;
- 8 are improvement candidates;
- 2 are regression candidates: four-pair owner-bound embedded GET (-17.1%) and four-pair raw
  read-after-write (-4.4%).

The intervening C ABI/artifact-delivery change also refactored paired-runtime and reactor sources,
so the candidates cannot be dismissed as documentation-only noise. The environment comparison is
nevertheless not admissible as a formal baseline, and the changes include broad mechanical
refactoring. The two microbenchmark candidates and the maintenance deterioration therefore require
controlled A/B isolation before a causal claim or rollback decision.

## Priority order

1. **P0 — incremental maintenance/publication quanta.** Bound publication lease time and forced
   rotation interference; expose an explicit pause budget and verify it under overlap.
2. **P0 — publication batching and fairness.** Close batches on queue pressure, bytes and bounded
   deadlines while preserving visible-before-ACK and durability ordering.
3. **P0 — durable queue/commit attribution.** Preserve separate queue, service, publication and
   physical commit timing; tune barriers only on real platform/filesystem/device rows.
4. **P1 — component memory accounting.** Attribute immutable base, delta, active Segment, retired
   generations, connection buffers and allocator slack; then reduce the measured amplification.
5. **P1 — large-value network ownership.** Retain generation pins through vectored output and make
   copied bytes/backpressure observable without weakening bounded memory.
6. **P1 — controlled benchmark row.** Add isolated Linux bare metal, physical-core pinning,
   `perf stat`, allocator/copy counters and separate client/server placement.

## Artifacts

- `commands.txt`: exact 219-command ledger.
- `environment.txt`: source, toolchain, host and campaign identity.
- `results.json` / `results.md`: strict normalized core/scaling/TCP/durable report.
- `core/`, `scaling/`, `tcp/`, `durable/`: standard raw benchmark output.
- `paired/`: paired shard and paired reactor A/B output.
- `compaction/`: durable compaction CSV series.
- `maintenance/`: mixed, rotation, churn and idle CSV series.
