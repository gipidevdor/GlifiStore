# Paired embedded Store gate snapshot

**Historical** · macOS arm64, `macos-release`, Apple LLVM 21 · 2026-08-01

ADR 0032 product default verified on `main` (`92cb5d2` + ctor-order fix). Warmup 1, repeats 3
(median).

## Correctness / sanitizers

- `./scripts/dev.sh test` — **37/37** ctest (includes `paired_store_tests`, parallel bench smokes).
- Prior session: ASan+UBSan and TSan suites green on paired default (`92cb5d2`).

## Latency sanity (durable parallel, Zipf, 4 workers × 4 threads, 2000 ops)

| Workload | median ops/s | p50 | p99 |
|----------|-------------:|----:|----:|
| `store-durable-parallel-get` (copy) | ~372k | ~2.1 µs | ~40 µs |
| `store-durable-parallel-put` (sync durable) | ~328 | ~11 ms | ~27 ms |

GET stays in the microsecond band under concurrent writers on other keys / Zipf skew. PUT p99 is dominated by `durable_sync` fsync cost, not Writer mutex contention.

Volatile `store-parallel-all` (Zipf, 4×4, 512–5000 ops) completes without failure across repeated smokes.

## Residual (honest)

- Catalog `shared_lock` may still be taken on durable mutate/capture for pin lookup.
- `durable_group` / `durable_periodic` keep Worker mutex when a background flusher shares state.
- Linux hard-pinned A/B remains the stronger production claim path (`scripts/benchmark_paired_linux_ab.sh`).
