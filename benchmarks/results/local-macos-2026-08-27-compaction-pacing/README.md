# Compaction physical-write pacing — local macOS A/B

Status: **local evidence only**. This is not a release or cross-platform performance claim.

## Host and build

- Date: 2026-08-27 (Europe/Rome)
- Host: Apple M4, 10 physical/logical CPUs, 16 GiB RAM
- OS: macOS 26.6.2 (Darwin 25.6.0, arm64)
- Source: `5feccd33b518e73bb5ac96ad3658fb2633020613` plus the documented dirty worktree
- Build: `build/macos-native-release`
- Execution: four variants run sequentially; two warmups followed by five measured repeats each

The worktree includes ADR 0039 private pre-intent staging and the ADR 0040 candidate. Results must
be compared only within this same-host run. Earlier accidentally concurrent exploratory samples are
excluded.

## Workload

`glifistore_maintenance_benchmark --scenario mixed --operations 15360 --threads 4 --keys 128
--value-bytes 65536 --reclaim-value-bytes 262144 --put-percent 5 --maintenance-interval-ms 10
--cooldown-ms 250 --warmup 2 --repeats 5`

- durable-periodic Store, two shard pairs;
- reclaim candidate on shard 0, foreground 95% GET / 5% PUT on shard 1;
- 32,515,776 live bytes copied and three Segments left after useful compaction;
- variants: maintenance disabled; background unlimited; background 64 MiB/s; background 128 MiB/s.

## Median result

| Variant | Foreground ops/s | p99 µs | max µs | pacing delay ms | sleeps | burst bytes |
|---|---:|---:|---:|---:|---:|---:|
| disabled | 115,086 | 419.250 | 4,917.166 | 0 | 0 | 0 |
| background unlimited | 94,242 | 603.459 | 9,144.375 | 0 | 0 | 0 |
| background 64 MiB/s | 105,731 | 435.750 | 4,270.292 | 446.560 | 123 | 671,089 |
| background 128 MiB/s | 112,250 | 457.208 | 4,585.042 | 212.994 | 123 | 1,048,576 |

Relative to unlimited background, 64 MiB/s improved median foreground throughput by **12.19%**,
reduced p99 by **27.79%**, and reduced maximum latency by **53.30%**. At 128 MiB/s, throughput
improved **19.11%**, p99 fell **24.24%**, and maximum latency fell **49.86%** while private staging
delay was about half that of 64 MiB/s. The 128 MiB/s balance came within **2.46%** of
maintenance-disabled throughput; its p99 remained **9.05%** above disabled.

## Interpretation and retained limits

The controlled result supports bounded in-flight I/O pacing rather than a whole-transaction rate
gate. It does not establish a universal default: filesystem cache, scheduler granularity and device
sharing differ by platform. The named daemon profiles therefore use overrideable starting points
(64 MiB/s embedded, 128 MiB/s production), pressure/emergency bypass pacing, and generic Store
configuration remains unlimited.

The benchmark records p99 but not p99.9 at this operation count, device queue depth, syscall count,
or native filesystem counters. Linux/FreeBSD/OpenBSD, shared-device, beyond-cache and multi-hour
reclaim-debt campaigns remain open. This evidence does not close `GATE-PERFORMANCE` beyond its
existing CI status and does not change the architectural-prototype claim ceiling.

## Verification ledger

See `verification.txt` after the final test, sanitizer, crash and assurance runs. Exact measured
medians are retained in `medians.csv`; per-repeat primary measurements are in `samples.csv`.
