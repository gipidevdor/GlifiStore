# Local volatile Reader copy comparison, 2026-11-21

Architectural prototype; uncontrolled macOS arm64 Release exploration for ADR 0048.
HAZ-045 and HAZ-046 remain open. This verifies a reduction in Reader work, not dynamic
redistribution, a saturation threshold, sustained sibling isolation or release acceptance.

## Change and method

The previous volatile TCP GET materialized an independent value and copied it again into
contiguous output. The candidate validates a borrowed Record under the existing exact Reader
generation lease and encodes small responses synchronously into owning output. Values above
64 bytes and below the unchanged pipelined-scatter threshold no longer need an intermediate
heap value. Large scatter responses and public Store results retain independent ownership.
The correctness tests retain CRC, identity, TTL, tombstone, generation-lifetime and pipeline checks.

The run order is `before` (old binary), `after` (candidate), then `control` (the identical old
binary again). Every pass has eight cases, one warmup and seven measured repeats. All raw
responses and values pass the strict owner-profile validator. `campaign.json` retains every
argv, raw checksum and binary SHA. `baseline-inputs.json` records the old build from base
commit f1e07b5; `candidate-inputs.json` records the dirty candidate's complete native input
fingerprints and Release flags. Its measured inputs were unchanged at campaign completion.
`verified-source-sha256.json` pins implementation and native test sources. The original
measurement runner's bounded case list is represented by the recorded commands; no durable
stores were created. The old executable is discarded after the checksums and outputs are retained.

There are two fixed-owner shards, distinct 16-byte keys and raw TCP loopback in the server
process. Pipeline depth is 32. Values are 64, 1024 or 4096 bytes. The 64-byte cases use 200,000
key operations; larger values use 100,000. GET-only emits one request per key operation;
read-after-write emits two. `hot0` is ordinary round-robin placement, `hot3` binds three of four
clients to Worker 0 (75% finite work), and `hot9` binds nine of ten (90%). They are finite work
ratios, not a maintained offered-rate ratio. Cold clients can finish early.

The observed machine has 10 physical and 10 logical CPUs. Four-client cases have a foreground
thread floor of eight; ten-client cases have a floor of fourteen and oversubscribe it. There is
no pinning or thermal/power control. The runs are sequential local observations, not randomized
trials or a statistical performance acceptance. Hardware observations and process-memory
measurements are retained. Initial compilation and validation precede these comparisons.

## Observations

Rates below are the common whole-sample median in wire requests per second. Full min/max and
per-owner repeat samples remain in raw files and `checked-summary.json`. Changes compare the
candidate median separately with each old-binary median; this interval is not a confidence
interval. Owner 0 p99 values are medians of per-repeat p99, in microseconds, in run order.

| Case | Before req/s | Candidate req/s | Old control req/s | Median change vs old runs | Owner 0 p99 before / candidate / control µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| `get-only-v1024-c4-hot0` | 1,398,950 | 1,545,620 | 1,402,120 | +10.2 to +10.5% | 127.0 / 117.1 / 127.4 |
| `get-only-v1024-c4-hot3` | 1,397,670 | 1,531,000 | 1,405,300 | +8.9 to +9.5% | 117.4 / 115.6 / 118.1 |
| `get-only-v4096-c4-hot0` | 642,955 | 674,383 | 641,077 | +4.9 to +5.2% | 230.5 / 225.4 / 231.1 |
| `get-only-v4096-c4-hot3` | 639,094 | 669,087 | 641,352 | +4.3 to +4.7% | 241.9 / 245.4 / 241.1 |
| `get-only-v64-c10-hot9` | 2,263,180 | 2,356,610 | 2,271,080 | +3.8 to +4.1% | 169.0 / 161.0 / 166.1 |
| `get-only-v64-c4-hot0` | 2,715,040 | 2,738,540 | 2,713,560 | +0.9 to +0.9% | 73.7 / 73.5 / 74.1 |
| `get-only-v64-c4-hot3` | 2,551,050 | 2,627,720 | 2,556,170 | +2.8 to +3.0% | 66.0 / 64.9 / 65.3 |
| `read-after-write-v64-c4-hot3` | 297,138 | 295,713 | 293,202 | -0.5 to +0.9% | 1179.5 / 1184.9 / 1181.6 |

The hot-owner 1 KiB case improves from about 1.40 to 1.53 M GET/s, with both old-binary controls
near 1.40 M. The four-client 64-byte gain is smaller and the balanced gain is marginal. Ten-client
numbers remain oversubscription sensitivity observations. The hot-owner 4 KiB throughput improves,
but its median p99 rises from about 241 to 245 microseconds; there is no across-the-board tail-latency
claim. Read-after-write is between the old runs, so no mutation improvement is demonstrated.
Finite loops do not demonstrate sustained offered-load capacity, single-hot-key isolation or
redistribution to idle shards. The serial Writer and fixed ownership remain unchanged limits.

## Retained verification

- `native-ctest.txt`: 788 native tests, plus all eight selected CTest groups passed (groups overlap).
- `allocation-failures.txt`: complete allocation-fault harness passed, including the 4 KiB borrowed
  read with allocations forbidden and the existing owning inline GET.
- `sanitizers.txt`: nine targeted tests passed under ASan and UBSan with halt-on-error, covering
  exact generation pins, corruption/TTL/tombstones, publication barriers and scatter boundaries.
- `tooling-validation.txt`: 474 script tests, three GDev metadata tests, pinned formatting,
  documentation, dependency/structure checks, and the memory-order audit passed. The audit retains
  1125 orders; only source identity and shifted line references changed.
- `verification.json`: raw and validator fingerprints and explicit non-acceptance of open hazards.

The first general validation rejected stale memory-order evidence after the source edit. The
inventory was regenerated and the validation repeated successfully. The formal workflow also
refused analysis until its project snapshot was linked; the required link was then recorded.
Those attempts remain in the local GDev audit archive. Human approval is not verified.

Native Linux/BSD performance and sanitizer rows remain unmeasured for this change. The returned
volatile view owns no pin; retaining its exact generation through every access is mandatory.
The TCP path finishes copying before the next adoption or quiescence and stores no borrowed
spans in asynchronous output. This evidence does not close a production gate.
