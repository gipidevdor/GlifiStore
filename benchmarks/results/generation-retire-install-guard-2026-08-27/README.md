# Generation retire install guard — local diagnostic A/B (2026-08-27)

Status: local diagnostic only. This is not release evidence and does not close ADR 0036 V11/V12.

## Scope

Official TCP runtime, volatile 90/10 GET/PUT, one shard pair, two owner-bound clients, pipeline 32,
200,000 operations per sample, 64-byte values, Apple arm64/macOS. Each direction used one warmup and
five measured repetitions. The control temporarily removed only the second, internal retire-bound
check from `install_writer_generation`; the normative pre-Store admission remained enabled. The
unsafe control never existed in the final tree.

## Result

The two order-balanced median pairs were:

- bounded guard: 693,045 and 693,972 ops/s;
- unsafe control: 707,756 and 701,935 ops/s.

The mean of paired medians is approximately -1.6% throughput for the guard. Paired p99 moved by
approximately +3.2%; p99.9 moved in the opposite direction and improved by approximately 2.9%.
A seven-repeat confirmation with the failure branch marked `[[unlikely]]` measured 697,110 ops/s,
p99 144.2 us and p99.9 372.1 us.

This effect is below the ADR 0036 5% throughput hard-reject threshold, but the run is too small and
the host too noisy to claim a precise overhead. The guard is retained because it converts a future
admission-bypass regression from silent unbounded ownership / invalid ACK behavior into an explicit
process-fatal invariant violation. It adds no allocation, lock or atomic operation and is outside GET.

Exact medians are retained in `ab.tsv`. A real ADR 0036 V11/V12 decision still requires the
production slot-pool candidate, worker-affine PUT 2-thread workload, controlled order, and the full
platform-specific benchmark protocol.
