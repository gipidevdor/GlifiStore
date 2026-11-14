# Reactor sliding input buffer advisory A/B

**Historical** · Apple Silicon arm64, macOS, AppleClang 21, Release · 2026-08-25

Accepted implementation signal, not a release gate. Baseline: `a02157e`. Candidate:
`codex/sliding-reactor-input` working tree.

The Reactor previously erased the consumed prefix after each asynchronous Store request. A fully
received pipeline therefore moved the remaining suffix once per completion. The candidate retains
an input cursor and compacts only when a later append would otherwise reallocate or cross
`maximum_input_bytes`. Wire ordering, request admission and the input watermark are unchanged.

## Comparable local cells

Raw-wire TCP, volatile paired Store, one shard pair, one client, 64-byte values, PUT→GET workload,
100,000 pairs, two warmups and seven measured samples per run. Candidate/baseline order was reversed
for the second pipeline-128 run.

| Pipeline | Baseline median | Candidate median | Delta |
| ---: | ---: | ---: | ---: |
| 1 | 45,798 ops/s | 46,381 ops/s | +1.3% |
| 128, run A | 229,287 ops/s | 233,206 ops/s | +1.7% |
| 128, run B | 230,024 ops/s | 233,919 ops/s | +1.7% |

The pipeline-128 candidate had noisy minimum samples on this non-isolated host, and the observed
ranges overlap. The repeated median is directionally positive and shows no median regression, but
the throughput delta is not a closed statistical claim. Absolute and tail-latency claims remain
gated on the labeled Linux performance runner. Correctness evidence is the deep ordered
mutation-pipeline integration test plus the normal partial-frame, slow-output, BSD and sanitizer
matrices.
