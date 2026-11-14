# GlifiStore local performance profile — 2026-08-26

## Verdict

This campaign confirms that the paired architecture has a strong read-side primitive,
but it is not yet an end-to-end performance win for mixed traffic. The immutable
`ReadGeneration`/span path removes the value-copy cost and is excellent in isolation.
The current publication policy then gives much of that gain back when writes are mixed
into the TCP reactor: batches are usually too small, generations are published too
often, and write-heavy tails grow quickly.

The next performance block should therefore optimize publication and reclamation
amortization while preserving the existing visibility, durability, bounded-memory and
fail-closed contracts. Replacing the index or adding lower-level SIMD work is not the
highest-impact next step.

These are local laboratory measurements, not release-gate evidence and not a claim of
production readiness.

## Measurement identity and scope

- Source: pre-commit dirty tree based on `f999cac7c526eb4e5b4a0283ec8c1d1db3e6ba86`.
- Host: Apple M4, 10 physical/logical cores, 16 GiB RAM, macOS 25.6.0, APFS SSD.
- Build: Release, AppleClang 21, no LTO, no `-march=native`.
- Placement: client and server in the same process/host; macOS affinity is advisory.
- Sampling: normally one warm-up plus seven measured repetitions; durable and paired
  reactor cases use five repetitions where recorded in the source file.
- Retained data: 180 strict machine-parsed results plus paired A/B, compaction and
  maintenance CSV series. Invalid exploratory attempts are isolated in `diagnostics/`.
- Missing from this local row: Linux/FreeBSD/OpenBSD, hard CPU pinning, NUMA, hardware
  counters, isolated bare-metal client, long soak, thermal telemetry and an accepted
  historical baseline.

The APFS volume was 92% full during the campaign. Durable numbers are therefore useful
for identifying ordering and queueing behavior on this machine, but not for portable
absolute latency claims.

## Core engine

Median throughput, single worker, uniform distribution:

| Value | GET copy | PUT | PUT batch | PUT+GET | Read-after-write |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 16 B | 4.256 M/s | 512.4 k/s | 690.4 k/s | 880.0 k/s | 929.9 k/s |
| 64 B | 4.053 M/s | 501.0 k/s | 692.1 k/s | 882.6 k/s | 907.0 k/s |
| 256 B | 2.729 M/s | 501.4 k/s | 632.4 k/s | 830.4 k/s | 900.6 k/s |
| 1 KiB | 1.728 M/s | 446.8 k/s | 557.8 k/s | 703.7 k/s | 710.5 k/s |
| 4 KiB | 789.4 k/s | 299.4 k/s | 389.7 k/s | 434.1 k/s | 437.2 k/s |
| 64 KiB | 66.1 k/s | 61.2 k/s | 64.7 k/s | 60.6 k/s | 61.8 k/s |
| 256 KiB | 17.2 k/s | 15.7 k/s | 16.8 k/s | 17.0 k/s | 15.4 k/s |

The 64-byte index itself reaches 11.02 M hit lookups/s and 13.85 M miss lookups/s.
The complete store GET-copy path reaches only 4.05 M/s, so index probing is not the
dominant read cost. Ownership/lifetime handling and value copying are materially more
important.

The RSS measurements also support the existing memory-amplification concern. The
64-byte/200k-key run reports about 238 MiB RSS for about 12.2 MiB of logical values.
This is not a component-level allocation audit, because it includes the whole benchmark
process, but the ratio is large enough that compact metadata, segment sizing and removal
of duplicate hot representations remain P0/P1 work.

## Embedded shard scaling

| Pairs/workers | Owner GET | Uniform GET | Zipf GET | Owner PUT | Owner read-after-write |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.039 M/s | 3.646 M/s | 3.876 M/s | 578.2 k/s | 1.057 M/s |
| 2 | 8.668 M/s | 5.842 M/s | 6.019 M/s | 742.9 k/s | 1.189 M/s |
| 4 | 17.361 M/s | 6.807 M/s | 8.374 M/s | 1.135 M/s | 2.239 M/s |
| 8 | 21.282 M/s | 11.949 M/s | 10.615 M/s | 1.993 M/s | 3.268 M/s |

Owner-bound GET scales strongly through four pairs. Uniform and Zipf routing scale less
well, exposing the locality cost of non-owner access patterns. Eight pairs oversubscribe
this 10-core host once Reader and Writer threads are counted, so the 8-pair point is a
stress observation rather than the principal scaling baseline.

## Paired read primitive versus current Store

| Value | Workload | Paired | Current copy | Throughput ratio | Paired p99 GET | Current p99 GET |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | GET 100% | 13.324 M/s | 5.919 M/s | 2.25x | 125 ns | 416 ns |
| 64 B | GET 95% / PUT 5% | 11.173 M/s | 3.715 M/s | 3.01x | 167 ns | 500 ns |
| 1 KiB | GET 100% | 13.363 M/s | 2.209 M/s | 6.05x | 125 ns | 709 ns |
| 64 KiB | GET 100% | 14.283 M/s | 68 k/s | 209.7x | 125 ns | 16.958 us |
| 256 KiB | GET 100% | 13.894 M/s | 17 k/s | 813.4x | 583 ns | 81.542 us |

The large-value ratios compare a borrowed span with an owning value copy. They prove
the value of the no-copy lifetime design; they do not mean the network can deliver
hundreds of times more large-value responses. Socket framing, kernel copies and output
backpressure remain in the end-to-end path.

## TCP volatile profile

One pair, one connection, 64-byte values:

| Pipeline | Workload | Throughput | p50 | p99 | p99.9 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | GET | 46.1 k/s | 21.8 us | 37.0 us | 91.1 us |
| 8 | GET | 286.3 k/s | 25.9 us | 64.8 us | 156.6 us |
| 32 | GET | 887.1 k/s | 34.7 us | 42.5 us | 66.3 us |
| 128 | GET | 1.818 M/s | 65.2 us | 81.9 us | 94.5 us |
| 32 | GET 99% / PUT 1% | 737.8 k/s | 36.5 us | 78.2 us | 157.4 us |
| 32 | Read-after-write | 331.9 k/s | 117.8 us | 867.3 us | 2.962 ms |
| 128 | Read-after-write | 202.9 k/s | 485.8 us | 4.383 ms | 9.143 ms |

Pipelining is essential for throughput, but deep pipelines are actively harmful to
read-after-write tail latency. Pipeline 32 is the more balanced 64-byte operating point
on this host; pipeline 128 is useful for GET saturation, not as the default mixed profile.

At pipeline 32, GET throughput scales from 887 k/s at one pair to 1.606 M/s at two,
2.618 M/s at four and 3.662 M/s at eight. Read-after-write does not scale similarly:
its p99 rises from 0.867 ms at one pair to 2.349 ms at four and 4.831 ms at eight. The
mutation/publication/completion side, rather than socket GET dispatch, limits mixed
scaling.

Large-value GET reaches about 0.73 GiB/s duplex at 64 KiB/pipeline 32 and 0.79 GiB/s at
256 KiB/pipeline 8. A 256 KiB/pipeline 32 response burst exceeds the configured 8 MiB
output bound and is correctly rejected. During the campaign the seeding helper was also
fixed to cap seed batches at 2 MiB, preventing the setup phase from violating the 4 MiB
input bound; a regression test now covers the 256 KiB case.

## Paired versus current reactor A/B

Pure GET at useful pipeline depths is neutral to positive:

- one client: paired/current is 1.058x at pipeline 32 and 1.134x at pipeline 128;
- four clients: paired/current is 1.013x at pipeline 32 and 1.125x at pipeline 128.

Mixed traffic is the unresolved regression:

- four clients, pipeline 32: 0.941x at 1% PUT, 0.724x at 5% PUT, 0.426x at 10% PUT;
- four clients, pipeline 128: 0.985x at 1% PUT, 0.680x at 5% PUT, 0.470x at 10% PUT;
- at four clients/pipeline 32/10% PUT, paired p99 batch latency is 797 us versus 261 us.

Telemetry in the 1% PUT case reports an average publication batch of only 1.23 records
and a maximum Writer batch of four. The evidence is consistent with excessive fixed
publication cost per mutation. The next implementation should make publication
deadline/pressure-aware, batch completion work, and account bytes and generation-retire
debt without weakening read-after-write linearization.

## Durability

One pair, pipeline 32, 64-byte values:

| Workload | Mode | Throughput | p99 | p99.9 | Median recorded commit |
| --- | --- | ---: | ---: | ---: | ---: |
| GET | sync | 38.8 k/s | 1.35 ms | 1.52 ms | n/a in this path |
| GET | group | 42.1 k/s | 1.28 ms | 1.65 ms | 2.98 ms |
| GET | periodic | 47.7 k/s | 0.81 ms | 0.93 ms | 0.60 ms |
| GET 99% / PUT 1% | sync | 9.2 k/s | 27.76 ms | 28.11 ms | n/a in this path |
| GET 99% / PUT 1% | group | 12.6 k/s | 11.36 ms | 11.57 ms | 2.20 ms |
| GET 99% / PUT 1% | periodic | 27.2 k/s | 8.04 ms | 8.30 ms | 0.69 ms |
| Read-after-write | sync | 0.4 k/s | 165.06 ms | 178.37 ms | n/a in this path |
| Read-after-write | group | 0.3 k/s | 290.75 ms | 861.68 ms | 5.71 ms |
| Read-after-write | periodic | 17.1 k/s | 8.64 ms | 9.69 ms | 0.66 ms |

Periodic mode is faster because its acknowledgement contract is different; it must not
be presented as a substitute for synchronous durability. The sync and group
read-after-write tails are the critical result. On this APFS row, group batching does
not reliably amortize the workload: the batch sweep peaks at 20.7 k/s for a configured
batch of four, while configurations 16–128 observe only four records per median batch
and much worse tails. Device-aware commit coordination and deadline/fairness control
need measurement on real Linux filesystems before any default is changed.

## Compaction and maintenance

Compaction copies about 31 MiB in 188 ms for high reclaim, 127 MiB in 776 ms for medium
reclaim, 191 MiB in 1.176 s for low reclaim, and 255 MiB in 1.509 s for copy-heavy data.
The ordinary copy rate is approximately 160–200 MiB/s. All useful cases compacted and
reopened successfully in all seven repetitions; the no-gain case correctly declined the
rewrite in all seven repetitions.

For the mixed foreground workload, disabling maintenance reaches 109.7 kops/s with a
454 us p99. Cooperative maintenance reaches 83.6 kops/s/710 us and background reaches
87.2 kops/s/704 us while each completes one useful reclaim. This is the measurable cost
of reclaim, not a like-for-like regression, but it shows that maintenance is still a
major interference source.

Forced rotation is more concerning: median rotation is 110 ms with maintenance disabled,
191 ms cooperative and 221 ms background. Background mode spends a median 155 ms waiting
for publication. Maintenance remains too monolithic around rotation/publication and must
be split into bounded quanta with explicit pause budgets.

Idle background maintenance is inexpensive on this host (median process CPU duty about
0.005%), so the issue is overlap and critical-path coordination, not idle polling.

## Priority order from the evidence

1. **P0 — publication amortization and fairness.** Increase useful Writer batches under
   mixed load using queue pressure, byte thresholds and bounded deadlines; retain the
   exact visibility/ACK contract and bounded queues.
2. **P0 — bound publication/rotation interference.** Remove the 140–155 ms publication
   waits seen during forced rotation; maintenance work must yield at explicit quanta.
3. **P0 — durable queue/commit behavior.** Separate and time queue wait, service,
   publication and physical commit on every path; tune group policy per platform only
   after Linux filesystem/device campaigns.
4. **P1 — memory accounting and reduction.** Add per-component bytes for immutable base,
   frozen delta, active segment, retired generations, connection buffers and allocator
   slack; then reduce slot/segment/hot-cache duplication with a hard memory budget.
5. **P1 — network large-value path.** Preserve generation pins through vectored output,
   reduce framing copies, and make read-interest/output-cap backpressure observable.
6. **P1 — benchmark isolation.** Add an isolated Linux bare-metal row with physical-core
   pinning, `perf stat`, allocator/copy counters and client/server separation. Retain this
   macOS row as exploratory evidence, not as an accepted cross-platform baseline.

## Artifacts

- `results.json`: normalized machine-readable core/TCP/durable results.
- `results.md`: complete generated table for all normalized results.
- `environment.txt`: source and machine identity.
- `commands.txt`: executed command ledger, including superseded exploratory attempts.
- `core/`, `scaling/`, `tcp/`, `durable/`, `paired/`, `compaction/`, `maintenance/`:
  accepted raw measurements.
- `diagnostics/`: explicitly rejected or intentionally bounded cases; excluded from the
  normalized report and conclusions.
