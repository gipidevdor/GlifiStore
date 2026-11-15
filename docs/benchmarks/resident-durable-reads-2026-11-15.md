# Resident durable reads against the ADR 0045 targets

**Advisory** · Apple M4 (10 cores, 16 GiB), macOS 27.2, AppleClang, Release · 2026-11-15

Local campaign for the [ADR 0045](../adr/0045-memory-resident-durable-reads.md) benchmark targets,
with the [ADR 0046](../adr/0046-unified-memory-reserve.md) reserve at its 1 GiB default. Source:
`7973c9d` plus `--cold-reads` in the server benchmark. Not release evidence: one non-isolated host,
no retained raw output.

Shape of every cell: 16-byte keys, 64-byte values, `durable_periodic` for durable rows (reads take
the same path in every durable mode), 100,000 operations, one warmup and seven measured samples,
median ops/s. *Resident* uses Segment images; *cold* passes `--cold-reads`, so every GET reads the
Segment file through the page cache.

## Verdict

| Target | Measured | Status |
| --- | --- | --- |
| Embedded resident GET ≥ 90% of volatile | 61% | Not met |
| Daemon resident GET-only, pipeline 32, ≥ 80% of volatile | 75% (1 pair), 78% (4 pairs) | Not met |
| Fully cold daemon GET grows with pipeline depth | +16–26% from pipeline 8 to 128 (volatile: 8–9×) | Not met in substance |
| Durable PUT not regressed | 9.03 µs resident, 9.07 µs cold | Met |
| RSS reported against the reserve | +11 MiB for 100,000 Records, reserve 1 GiB | Reported |

Resident reads still change the daemon by an order of magnitude: against the ADR 0045 baseline,
GET-only at pipeline 32 rises from 40 k/s to 668 k/s with one pair and from 82 k/s to 2.35 M/s with
four.

## Daemon GET-only

Raw-wire TCP, paired Store, one client per pair.

| Pairs | Pipeline | Volatile | Resident | Cold |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 36 k | 39 k | 19 k |
| 1 | 8 | 196 k | 207 k | 41 k |
| 1 | 32 | 899 k | 668 k | 48 k |
| 1 | 128 | 1.76 M | 1.31 M | 52 k |
| 4 | 1 | 109 k | 115 k | 60 k |
| 4 | 8 | 754 k | 725 k | 103 k |
| 4 | 32 | 2.66 M | 2.35 M | 110 k |
| 4 | 128 | 5.94 M | 4.41 M | 119 k |

Pipeline 32 repeated three more times, alternating volatile and resident: resident was 71–79% of
volatile with one pair and 76–84% with four. An earlier single run at 200,000 operations measured
81%; the repeats show it was the high end of the noise.

Up to pipeline 8 resident matches volatile. The gap opens where the Reader thread is CPU-bound: on
top of the value copy, a resident GET decodes and validates the Record (CRC32C, identity, key and
expiry) and builds an owned response. Which part dominates is not profiled yet.

The cold path saturates near one read per connection. Each connection keeps a single cold read in
flight and stops decoding until it completes, so its rate is bounded by one read's round trip
through the disk-read lane. ADR 0045 §3 decides up to 32 cold reads in flight per connection with
ordered response slots; that is not implemented yet.

## Embedded GET

`glifistore_benchmarks --filter store-get` and `--filter store-durable-periodic-get [--cold-reads]`,
one thread, two warmups, three runs.

| Path | Median ns per GET | Relative to volatile |
| --- | ---: | ---: |
| Volatile | 233 | 100% |
| Resident | 385 | 61% |
| Cold | 1 046 | 22% |

## Durable PUT

`--filter store-durable-periodic-put [--cold-reads]`, three runs: 9.03 µs resident and 9.07 µs cold
per PUT (median of run medians). Mirroring the Record into the image costs less than run-to-run
noise. Peak RSS was 53.5 MiB resident and 42.6 MiB cold.

## Follow-ups

1. Cold-read pipelining per connection (ADR 0045 §3), with ordered responses, cancellation and the
   output watermark.
2. Profile the resident GET (Record validation, owned copy, pin) against volatile, embedded and in
   the daemon, and remove what is not required by the read contract.
3. The same cells on the labeled Linux runner, retained as evidence.
