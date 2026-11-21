# Local per-owner TCP exploration, 2026-11-21

Scope is an uncontrolled macOS arm64 Release measurement of the architectural prototype.
HAZ-045 and HAZ-046 remain open; this campaign does not implement or accept balancing or routing changes.
The earlier HAZ-044 residual paths are unchanged. This is evidence for measurement tooling, not hardware
acceptance, saturation capacity, sustained isolation or a release claim.

## Inputs and method

The retained `campaign.json` records the complete base commit, dirty source/binary SHA-256 inventory,
compiler build flags, CPU, memory, power and thermal observations and every command. The raw `.txt`
files and checked `.json` reports share their case names. `verification.json` pins the current report
validators and checksums of every accepted raw output. All captured measurement inputs were unchanged
at the end of the campaign. The common hosted-contract checker was additionally strengthened after
capture to reject these opt-in profiles; it is outside the native measured code.

Every case uses two fixed-owner shards, same-process TCP loopback, 200,000 distinct key operations,
16-byte keys, 64-byte values, pipeline 32, one warmup and seven measured repeats. GET-only emits
200,000 requests per repeat; read-after-write emits 400,000. All expected responses and values were
validated. Four clients use 2:2 or 3:1 bindings. Ten clients use 5:5 or 9:1 bindings. These are exact
finite-work ratios, not a constant offered arrival ratio.

The machine reported 10 physical and 10 logical CPUs. The minimum foreground-thread floor is 8 for
four clients and 14 for ten clients. The ten-client rows are oversubscription sensitivity measurements,
not physical-core scaling evidence. There is no CPU pinning or thermal/power control. Only volatile
storage is measured, with no durable test stores.

## Observations

Throughput is the common whole-sample median and range. Per-owner p99 values are medians of each
repeat's p99; window values are medians of shared-start-to-last-client completion times in seconds.
For GET-only, latency covers GET responses. Read-after-write covers both PUT and GET responses.
These populations are not interchangeable. The request clock begins when each pipeline batch is sent.

| Case | Aggregate median op/s | Aggregate min–max op/s | Owner 0 p99 µs | Owner 1 p99 µs | Owner 0 / 1 window s |
| --- | ---: | ---: | ---: | ---: | ---: |
| `get-only-c10-hot0` | 3,537,760 | 3,501,580–3,551,860 | 113.0 | 113.8 | 0.0565 / 0.0565 |
| `get-only-c10-hot9` | 2,288,550 | 2,257,190–2,307,190 | 164.4 | 65.3 | 0.0874 / 0.0262 |
| `get-only-c4-hot0` | 2,710,330 | 2,161,580–2,846,120 | 77.0 | 74.4 | 0.0738 / 0.0734 |
| `get-only-c4-hot3` | 2,558,750 | 2,488,310–2,576,230 | 64.5 | 60.4 | 0.0781 / 0.0622 |
| `read-after-write-c10-hot0` | 455,177 | 447,607–459,856 | 2224.0 | 2176.4 | 0.8722 / 0.8787 |
| `read-after-write-c10-hot9` | 391,120 | 338,449–420,088 | 3578.0 | 1310.4 | 1.0227 / 0.6343 |
| `read-after-write-c4-hot0` | 311,684 | 309,849–312,566 | 1016.2 | 1011.0 | 1.2797 / 1.2833 |
| `read-after-write-c4-hot3` | 297,725 | 293,849–300,385 | 1158.8 | 1012.9 | 1.3435 / 1.3003 |

In the ten-client GET case, moving from 5:5 to 9:1 reduced the observed median from about 3.54 M to
2.29 M requests/s, while the hot owner's median p99 rose from about 113 to 164 microseconds. The
cold owner's shorter completion window shows why its improved p99 cannot establish sustained
isolation: its traffic stops before the hot owner's. Four-client GET throughput ranges overlap, so
their median difference is inconclusive. Read-after-write also shows allocation sensitivity, with
variance retained explicitly. These are workload observations, not a code regression comparison.

## Verification and failed attempts

- Four native planner tests cover exact and uneven quotas, alternate owners, missing siblings,
  invalid coordinates and maximum-size arithmetic.
- The real TCP checker covers five valid profile shapes and seven rejected argument sets, including
  an owner with only PUT requests and therefore zero eligible GET latency samples.
- Five targeted CTest groups pass, including the three existing TCP smoke paths and durable-group mode.
- Python tests reject partial matrices, duplicate coordinates, incorrect quotas/rates/latencies,
  swapped per-owner request mixes, overwritten evidence and executable changes during a campaign.
- Initial compilation exposed an unqualified `Result` name colliding with the harness type;
  qualification and a combined-include test fixed it. An initial profile check incorrectly assumed
  all mixed-workload responses had latency samples; the explicit population contract corrected it.
- Exploratory pre-final runs and failure logs are retained in the local GDev assurance archive.
  They are not pooled into this matrix or selected as performance baselines.

## Remaining work

A fixed offered-rate sweep, per-owner CPU/queue/admission decomposition, single-hot-key contention,
long sustained overlap, dedicated hardware and native Linux/BSD runs are still needed. Compaction
sustainability is a separate workload and is not proven here. Changes to ownership, forwarding,
rerouting or client retry semantics still require their architecture and compatibility work.
