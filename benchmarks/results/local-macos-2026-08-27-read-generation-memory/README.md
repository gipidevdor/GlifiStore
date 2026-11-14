# Immutable read-generation memory census — local macOS, 2026-08-27

Status: local engineering evidence, **not** a release gate or cross-platform claim.

## Environment

- Source: `5feccd3-dirty` on `codex/paired-write-pressure`
- Host: Apple M4, arm64, 16 GiB
- OS: macOS 26.6.2 (25G83)
- Compiler: Apple clang 21.0.0 (`clang-2100.1.1.101`)
- CMake: 4.4.0
- Preset/build: `macos-native-release`
- CPU pinning: unavailable/not requested on this host

The worktree contains the complete correction program in progress. These cells must be rerun from a
clean immutable commit before any release evidence claim.

## Reproduction

```bash
cmake --preset macos-native-release
cmake --build --preset macos-native-release --target \
  glifistore_memory_census_benchmark glifistore_benchmarks

build/macos-native-release/glifistore_memory_census_benchmark \
  --entries 200000 --key-bytes 16 --value-bytes 64 --workers 1
build/macos-native-release/glifistore_memory_census_benchmark \
  --entries 200000 --key-bytes 16 --value-bytes 64 --workers 4
build/macos-native-release/glifistore_benchmarks \
  --filter store-get --ops 200000 --key-size 16 --value-size 64 \
  --warmup 1 --repeats 7
```

Initial outputs are retained in `memory-census-w1.txt`, `memory-census-w4.txt`, and
`store-get-current.txt`. The bounded-mapping correction adds `memory-census-bounded-w1.txt`,
`memory-census-bounded-w4.txt`, `vmmap-before-after.txt`, and `allocation-policy-ab.txt`.

## Exact structural result

Both Worker counts produced an aggregate immutable-base capacity of 262,144 buckets and 196,608
base entries. The old layout's lookup-only counterfactual is exact from the removed representation:
one control byte, one 64-bit hash, and one pointer per bucket = 17 bytes. The new representation is
one control byte plus one `uint32_t` record index = 5 bytes. The full 64-bit hash resides in the
existing 64-byte compact record and full hash/key equality remains authoritative.

| Metric | Old counterfactual | Current | Difference |
| --- | ---: | ---: | ---: |
| Lookup bytes / allocated bucket | 17 | 5 | −12 (−70.59%) |
| Lookup bytes at capacity 262,144 | 4,456,448 | 1,310,720 | −3,145,728 (−3.00 MiB) |
| Compact record bytes / live base row | 64 | 64 | 0 |

This structural result does not depend on noisy elapsed-time or RSS sampling.

## Baseline live/reserved lower bounds versus allocator and RSS

| Workers | Logical payload | Attributed live lower bound | Attributed reserved lower bound | Allocator in use | Unattributed allocator in use | Allocator reserve slack | RSS | Unattributed RSS | Retired | Active merges |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 16,000,000 | 59,066,650 | 98,971,418 | 99,213,824 | 242,406 | 30,809,600 | 212,221,952 | 153,155,302 | 1 | 0 |
| 4 | 16,000,000 | 59,454,872 | 300,673,944 | 301,005,056 | 331,112 | 38,733,568 | 103,514,112 | 44,059,240 | 4 | 0 |

The live lower bound includes the mutable Index allocation payload, used Segment bytes, the current
immutable generation, and mutation payload arena storage. The reserved lower bound substitutes full
Segment capacity for used Segment bytes. On this macOS allocator the reserved lower bound explains
all but 0.24–0.33 MiB of the allocator's current in-use bytes. This identifies fixed 64 MiB Segment
reservation per shard as the dominant virtual allocation at four shards; those untouched pages are
lazy and are not equivalent to resident memory.

Both lower bounds intentionally exclude allocator/`shared_ptr` control blocks, thread stacks, loaded
images, and other process allocations. The native allocator also retains 29.38 MiB (one shard) or
36.94 MiB (four shards) between its current in-use and reserved byte counters. RSS can exceed the
current allocator reservation after transient publication/merge dirtying, or remain below virtual
Segment reservation when pages were never touched. Therefore `unattributed_rss_bytes` is a
diagnostic residual, not proof of a leak and not directly additive with allocator virtual bytes.

The large one-shard RSS residual was not explained by current untracked heap (under 0.3 MiB), live
retired-generation count, or a merge active at sampling time. `vmmap` then identified 133.5 MiB in
19 empty `MALLOC_LARGE` regions. Their 2.5–11.5 MiB progression matched successive immutable-base
record-array merge sizes.

## Bounded geometric mapping correction

Arrays at or above 1 MiB now use guarded anonymous mappings. Each immutable-base lineage owns at
most one spare geometric size-class mapping: same-class rebuilds alternate two buffers, while a
class change or lineage destruction unmaps the spare. The pool is not thread-local, so arbitrary
embedded callers cannot multiply retained mappings. Its mutex is touched only on base-array
allocation/retirement, never by GET or ordinary Delta publication.

| Workers | RSS before | RSS bounded | Delta | Current mapped payload | Spare mapping capacity | Allocator bytes in use | Unattributed allocator |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 212,254,720 | 92,831,744 | −56.26% | 12,582,912 | 16,777,216 | 86,631,024 | 242,502 |
| 4 | 103,514,112 | 93,306,880 | −9.86% | 12,582,912 | 16,777,216 | 288,422,592 | 331,496 |

The one-shard `vmmap` inspection fell from 133.5 MiB / 19 empty large allocator regions to 12 MiB /
2 regions. The final mapping state was two 16 MiB virtual payload classes: the current base had
12 MiB resident and the spare 11.5 MiB resident. This is bounded by lineage and size class; it is
not an unbounded allocator cache.

## Allocation-policy A/B

Same-source native binaries isolated general allocation from the bounded pool. Each cell used
200,000 operations, one Worker, one warmup and seven repeats.

| Workload | General allocator | Bounded pool | Throughput delta | RSS delta |
| --- | ---: | ---: | ---: | ---: |
| `store_put_batch` | 633,447 ops/s | 631,507 ops/s | −0.31% | −50.51% |
| `store_get_copy` | 4,327,140 ops/s | 4,351,980 ops/s | +0.57% | −49.40% |

Both throughput deltas are within the local uncontrolled spread. A rejected immediate-unmap
variant measured only 550–564 k batch PUT ops/s, so deterministic unmap on every retirement was not
retained. This remains **local dirty-source engineering evidence**, not a clean-commit,
cross-platform performance gate.

## Residual work

1. Count unique live allocations across retired/shared generations and in-progress merge builders
   without double-counting persistent nodes.
2. Attribute the remaining 4/8 MiB native allocator regions and thread/Segment residency.
3. Run steady-state insert/update/merge cycles long enough to prove the per-lineage bound under
   repeated size-class transitions and concurrent shards.
4. Repeat on Linux and supported BSD/macOS rows, with clean commits and controlled hosts.
5. Define enforceable per-shard memory budgets only after those measurements.
