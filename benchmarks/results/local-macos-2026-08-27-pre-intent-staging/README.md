# Durable compaction pre-intent staging

Date: 2026-08-27  
Host class: local Apple Silicon, macOS, APFS  
Build: native Release, dirty working tree at `5feccd3`  
Status: diagnostic evidence only; not release or cross-platform evidence

## Change under test

Replacement Segment copy, synchronization, seal and verification now complete under private
temporary names before the v1 compaction intent and global Manifest publication lease. Promotion
uses the existing intent v1, descriptor-relative renames, and one directory sync. Placement
metadata stores a source-vector index instead of duplicating an owning `IndexEntry`.

Command:

```text
build/macos-native-release/glifistore_compaction_benchmark --warmup 1 --repeats 5
```

Baseline: `../local-macos-2026-08-27-full-31bd35f-dirty/compaction/` (seven repeats, same host class,
same 256 KiB value size and public `Store::compact()` harness).

## Median result

| Scenario | Baseline compact ms | Staged compact ms | Change | New lease ms | Lease / total |
|---|---:|---:|---:|---:|---:|
| high-reclaim | 282.345 | 156.094 | -44.7% | 32.261 | 20.7% |
| medium-reclaim | 825.226 | 365.997 | -55.6% | 34.893 | 9.5% |
| low-reclaim | 1142.956 | 512.891 | -55.1% | 34.691 | 6.8% |
| copy-heavy | 1516.775 | 709.832 | -53.2% | 34.511 | 4.9% |
| ttl-50 | 839.059 | 430.734 | -48.7% | 34.910 | 8.1% |
| no-gain | 132.713 | 133.607 | +0.7% | 0 | 0% |

The useful-compaction median improved in every measured shape. The no-gain scan, which creates no
outputs and never acquires the lease, is effectively unchanged. These local timings are consistent
with eliminating one initial file sync and per-output creation directory sync, but they are not a
controlled filesystem/device certification.

The decisive result is the critical-window measurement: useful workloads spend a median
approximately 32.3–34.9 ms in the post-staging lease even when total compaction takes 156–710 ms.
Before ADR 0039 the lease structurally enclosed the complete scan/copy/build; the old harness did
not instrument that internal boundary, so no fabricated old lease number is reported.

The reported transient metadata lower bound is 0.280–0.494 MiB for useful scenarios. It proves the
fixed structures are bounded and records the compact placement representation, but it excludes
copied-string heap allocations, allocator metadata, preallocated Segment mappings/blocks, hot cache,
and the rest of process RSS. It therefore does not close the broader memory-amplification concern;
a native allocation/RSS census remains required.

## Correctness evidence run with this change

- 52 compaction-filtered unit/integration tests passed; the renamed concurrent-backup fence test
  passed separately and ten consecutive repetitions passed in the normal build.
- The standard process-kill matrix passed with updated pre-intent, promotion, old/next authority,
  partial rollback/retirement, and cleanup boundaries.
- The complete main test binary passed 627/627.
- The same 52 compaction tests, all three staged-Segment tests, and the focused concurrent-backup
  fence test passed under both ASan and TSan.
- Store, wire and daemon backup process-kill targets passed after the counted-fence correction.
- Assurance schema and referential validation passed with zero warnings.

The earlier combined CTest run covered every target; after the final ownership/fence corrections,
the affected main, sanitizer, persistence-crash and backup-crash targets were rerun explicitly.
This folder does not claim evidence from platforms other than this local macOS/APFS row.
