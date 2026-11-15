# Benchmark records

Dated measurements and the decisions they supported. Each record states what was measured and what
was decided; not every record names its host or commit. Records are advisory, not release evidence:
the [benchmark standard](../spec/benchmark-standard.md) defines what a publishable result requires,
and the [performance budgets](../assurance/performance-budgets.md) define the regression gates.

## Plan and harness

| Page | Settles |
| --- | --- |
| [Paired shards plan](paired-shards-plan.md) | Benchmark gates for the paired Reader–Writer runtime |
| [Linux hard-pinned A/B harness](paired-shards-linux-p1.md) | The harness for the open Linux A/B gate |

## Records

| Date | Record | Verdict |
| --- | --- | --- |
| 2026-11-15 | [Resident durable reads against ADR 0045](resident-durable-reads-2026-11-15.md) | Two of five targets met |
| 2026-08-25 | [Reactor sliding input buffer](reactor-sliding-input-2026-08-25.md) | Accepted signal, not a release gate |
| 2026-08-01 | [Paired embedded Store gates](paired-embedded-store-gates-2026-08-01.md) | ADR 0032 default verified |
| 2026-07-31 | [Delta directory-chunk COW](paired-delta-directory-chunks-2026-07-31.md) | Accepted; magnitude mixed on macOS |
| 2026-07-31 | [`get-into` and multi-extent scatter](paired-get-into-multi-extent-reject-2026-07-31.md) | Rejected |
| 2026-07-29 | [Borrowed cold-read lease](paired-borrowed-cold-read-2026-07-29.md) | Accepted |
| 2026-07-29 | [Inline cold-read slots](paired-inline-cold-read-2026-07-29.md) | Accepted |
| 2026-07-29 | [Cold-read scatter output](paired-scatter-output-2026-07-29.md) | Accepted, adaptive and confined |
| 2026-07-29 | [Durable cold-read audit](paired-durable-cold-read-2026-07-29.md) | Audit; scalability gate open |
| 2026-07-29 | [Durable generation refresh](paired-generation-refresh-2026-07-29.md) | Audit |
| 2026-07-29 | [Mutation slot, key and value arena](paired-mutation-arena-2026-07-29.md) | Pass |
| 2026-07-29 | [Generational Delta arena](paired-generational-delta-arena-2026-07-29.md) | Accepted, with a performance follow-up |
| 2026-07-29 | [Delta layout](paired-compact-delta-2026-07-29.md) | Rejected |
| 2026-07-29 | [Compact Base Index](paired-compact-read-index-2026-07-29.md) | Implemented; Reader gate open |
| 2026-07-29 | [Incremental Delta-to-Base merge](paired-incremental-merge-2026-07-29.md) | Implemented; p99.9 GET gate open |
| 2026-07-28 | [Production GET A/B](paired-production-get-2026-07-28.md) | Audit; gate not closed |
| 2026-07-25 | [Durable GET SIMD hot-cache probe](get-path-hot-cache-simd-2026-07-25.md) | Probe |

Raw local output stays out of Git; retained campaign evidence lives under
[`engineering/evidence/`](../../engineering/evidence/README.md).
