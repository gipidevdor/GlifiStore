# Paired borrowed cold-read lease

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Accepted as a P0 sub-block for safety and ownership.** The paired daemon's cold GET no longer
copies `PublishedReadRecord` and no longer increments the file pin `shared_ptr` per request. The
task carries a trivially copyable view of the key, `RecordRef` and generation, valid only within a
Reader-private epoch lease. The public and legacy path of `Store::get` keeps the owning token: the
borrowed lifetime did not become an API contract.

The performance result is positive at pipeline 1 and essentially neutral at pipeline 32. The high
percentiles of the unpinned macOS run are mixed: no tail-latency improvement is attributed to the
change. The block is promoted because it removes the refcount and the key duplication while keeping
an explicit, bounded lifetime proof, not because it shows a throughput jump on its own.

## Lifetime protocol

- the Reader adopts a single `PairReadGeneration` per event-loop turn;
- before transferring the cold task it records the epoch in a local tracker, without atomics or
  allocations;
- the Writer publication observes `min(adopted epoch, cold epoch still in flight)` as
  `reader_safe_epoch`;
- the retire list frees only generations with `retired_epoch < reader_safe_epoch`;
- the task can therefore borrow key storage, pin descriptors and the file generation from the
  immutable generation, without a per-request owner;
- the I/O helper materializes an `OwnedValue`; only after the completion does the Reader release the
  lease;
- a closed connection cancels the task but does not release the lease early;
- shutdown drains all cold completions before stopping the Writer and the Store.

The tracker has 65 cells (at most `64` retired generations plus the current one). Saturation or
overflow produces overload, never dynamic growth or forced reclamation. The deterministic test keeps
a borrowed task across compaction, refresh, source retirement and completion; it then verifies that
the generation is reclaimed only after the safe epoch is explicitly advanced.

## Benchmark

macOS arm64, Apple LLVM 21, Release, one pair, 8 clients, key 16 B, value 64 B, durable-periodic
1000 ms. Baseline: cold slot pool with owning pin. Candidate: the same structure with QSBR lease and
borrowed pin. The raw results are in `benchmarks/results/paired-borrowed-cold-read-2026-07-29/`. The
runs are sequential and `cpu_pin_applied=0`; small differences and temporal RSS variations are
indicative.

| workload | metric | owning slot | borrowed lease | delta |
|---|---|---:|---:|---:|
| GET p32 | throughput | 185,892/s | 186,012/s | +0.06% |
| GET p32 | p50 | 909.3 µs | 870.3 µs | −4.28% |
| GET p32 | p95 | 1.628 ms | 1.634 ms | +0.36% |
| GET p32 | p99 | 1.978 ms | 2.018 ms | +2.04% |
| GET p32 | p99.9 | 2.268 ms | 2.453 ms | +8.14% |
| GET p32 | median RSS | 75.64 MB | 74.65 MB | −1.32% |
| GET p1 | throughput | 106,058/s | 112,984/s | +6.53% |
| GET p1 | p50 / p99 | 73.3 / 112.1 µs | 69.9 / 109.3 µs | −4.60% / −2.49% |
| GET p1 | p99.9 | 134.8 µs | 133.0 µs | −1.30% |
| GET p1 | median RSS | 74.37 MB | 80.13 MB | +7.75% |
| 99/1 p32 | throughput | 170,859/s | 173,874/s | +1.76% |
| 99/1 p32 | p50 / p99 | 787.0 µs / 3.791 ms | 752.8 µs / 3.809 ms | −4.34% / +0.49% |
| 99/1 p32 | p99.9 | 5.325 ms | 5.439 ms | +2.14% |
| 99/1 p32 | median RSS | 73.45 MB | 78.95 MB | +7.49% |

The p32 profile is the most stable comparison: throughput and RSS stay neutral, while p99.9 worsens
in a run with uncontrolled spread. Pipeline 1 shows the expected benefit of removing the refcount
and the record copy (+6.5%), but RSS across processes is not repeatable. The definitive tail gate
requires interleaved A/B on hard-pinned Linux; these data do not support any claim about p99.9.

## Safety gate

- Debug suite: 459/459;
- allocation fault injection: pass;
- ASan + UBSan: 459/459;
- TSan: 459/459;
- strict ISO C++23/hardening build: pass;
- no change to Manifest, Segment, Record, routing or wire format.

## Next gate

The follow-up is documented in `paired-scatter-output-2026-07-29.md`: large non-pipelined cleartext
responses use a bounded lease and no longer copy `OwnedValue` into the connection buffer. TLS and
pipelining stay owning and contiguous, by contract and by A/B result. The preallocated pool for
mutation keys and values remains P0; `get-into` is a possible later optimization, not a shortcut
around lifetimes.
