# Paired inline cold-read slots

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Accepted as a P0 sub-block.** `PreparedColdRead` no longer allocates a pImpl per request. The
move-only state with the exact pin of the file generation lives inline in a bounded pool per lane;
the two SPSC queues carry only slot indices. The pin refcount and the value copies remain open: this
block does not declare the cold-read lease protocol complete.

## Ownership and synchronization

- the Reader is the only producer of `submitted` and the only consumer of `recycled`;
- the I/O helper is the only consumer of `submitted` and the only producer of `recycled`;
- every index identifies a stable preallocated slot;
- the release/acquire of `submitted` transfers the task, `RecordRef` and owning pin to the consumer;
- the consumer publishes the completion, destroys the task, returns the slot with release and only
  then wakes the Reader;
- shutdown drains the lane and destroys every pin before `Store::close`.

No `RecordRef`, file handle or raw Segment generation is exposed without an owner. The inline
storage uses explicit construction/destruction, with size and alignment verified at compile time.

## Benchmark

macOS arm64, Apple LLVM 21, Release, one pair, 8 clients, key 16 B, value 64 B, durable-periodic
1000 ms. The raw results are in `benchmarks/results/paired-inline-cold-read-2026-07-29/`.

The closest, most homogeneous comparison uses 4 warm-ups and 7 samples, pipeline 32:

| metric | pImpl baseline | inline + slot pool | delta |
|---|---:|---:|---:|
| throughput | 186,431 ops/s | 185,892 ops/s | −0.3% |
| p50 | 876.8 µs | 909.3 µs | +3.7% |
| p95 | 1.605 ms | 1.628 ms | +1.4% |
| p99 | 1.969 ms | 1.978 ms | +0.4% |
| p99.9 | 2.291 ms | 2.268 ms | −1.0% |
| median RSS | 74.29 MB | 75.64 MB | +1.8% |

All the temporal differences are within the expected noise of the macOS run without hard affinity.
The first inline candidate, which moved the whole payload into the ring, had p99 +9.5%, p99.9 +21.6%
and RSS +7.5%; it was replaced by the slot pool and not accepted in that form.

Additional current profiles:

- pipeline 1: 106,058 ops/s, p50 73.3 µs, p99 112.1 µs, p99.9 134.8 µs;
- 99/1 GET/PUT pipeline 32: 170,859 ops/s, p50 787.0 µs, p99 3.79 ms, p99.9 5.33 ms.

The historical comparisons of these two profiles also include the intermediate Base/merge blocks and
are therefore kept only as a non-regression check, not as causal attribution.

## Safety gate

- CTest 37/37, including 8 crash matrices and allocation fault;
- Debug suite 459/459;
- ASan/UBSan 459/459;
- TSan 459/459;
- clean `unix-strict` build;
- covered: cold-read saturation, late completion on a reused connection slot, concurrent source
  retirement and shutdown with in-flight pins.

## Next gate

Extend QSBR with an explicit lease for I/O and output, then make key/pin borrowed only within that
lease. Only after that can the `PublishedReadPin` refcount be removed. The materialization must move
to get-into/scatter-gather with a separate TLS path; a raw pointer without an epoch pin is not an
allowed shortcut.
