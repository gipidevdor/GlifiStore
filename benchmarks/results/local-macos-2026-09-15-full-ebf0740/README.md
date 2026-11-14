# Full local benchmark campaign — 2026-09-15

Campaign run from clean `main` at
`ebf0740aec62e205d49b6642ea22b8f73004ec39` (**GlifiStore** rename tip) on the same Apple M4 host class and
`macos-native-release` configuration used by the
[2026-08-30 `b971a15` campaign](../local-macos-2026-08-30-full-b971a15/README.md).
This is local performance evidence after the GlyphaStore → GlifiStore rename, not a CI baseline,
a cross-platform comparison, a production-capacity claim, or durability certification.

## Integrity and coverage

- 222/222 commands completed (`running=0 ok=220 fail=0 skip=2 finished_at=2026-09-15T10:59:34Z`);
- the command matrix is byte-for-byte identical after replacing only the output directory and
  binary names already present as `glifistore_*` in the prior matrix;
- Release, native CPU on, LTO off, Apple clang 21.0.0 (`clang-2100.3.34.2`), macOS 27.0,
  APFS, AC power;
- core Store and Index; owner-bound/uniform/Zipf scaling at 1/2/4/8 Workers;
- volatile TCP at pipeline 1/8/32/128, paired Reactor, sync/group/periodic durability;
- compaction, maintenance, forced rotation, churn, idle, and generation diagnostics;
- 222 raw source files; `results.md` / `results.json` aggregate 180 canonical + 108 specialized rows.

Exact invocations are in `commands.txt`, environment identity in `environment.txt`, raw output in
the workload subdirectories, and the generic aggregate in `results.md` / `results.json`.

Automated baseline deltas versus `b971a15` are **suppressed**: environment identity differs on
`kernel_release` (`27.0.0` vs `25.6.0`) and `compiler_identity` (clang patch
`2100.3.34.2` vs `2100.1.1.101`). Absolute medians below are therefore same-host snapshots, not
strict regressions against the prior campaign.

The aggregate report is intentionally not described as strict. The current strict parser still
rejects specialized compaction/maintenance formats for the same metadata gap noted in the
`b971a15` README.

## Absolute medians (selected)

### Embedded 64-byte paths

| Workload | Median |
| --- | ---: |
| Store GET copy | 3.86 Mops/s |
| Store PUT | 414.91 kops/s |
| Store PUT batch | 697.54 kops/s |
| Store PUT + GET | 766.31 kops/s |
| Store read-after-write | 833.98 kops/s |

### Owner-bound scaling (GET copy / PUT)

| Workers | GET copy | PUT |
| ---: | ---: | ---: |
| 1 | 4.36 Mops/s | 493.23 kops/s |
| 2 | 9.55 Mops/s | 683.44 kops/s |
| 4 | 17.65 Mops/s | 1.10 Mops/s |
| 8 | 24.63 Mops/s | 1.80 Mops/s |

### Volatile TCP GET-only, pipeline 32

| Reader–Writer pairs | Median |
| ---: | ---: |
| 1 | 829.78 kops/s |
| 2 | 831.15 kops/s |
| 4 | 1.50 Mops/s |
| 8 | 3.59 Mops/s |

## How to reproduce

```bash
export PATH="$HOME/Library/Python/3.13/bin:$PATH"
cmake --preset macos-native-release
cmake --build --preset macos-native-release --target \
  glifistore_benchmarks glifistore_server_benchmarks glifistore_paired_benchmark \
  glifistore_paired_reactor_benchmark glifistore_compaction_benchmark \
  glifistore_maintenance_benchmark glifistore_generation_publication_benchmark \
  glifistore_generation_shell_benchmark
bash benchmarks/results/local-macos-2026-09-15-full-ebf0740/run_campaign.sh \
  benchmarks/results/local-macos-2026-09-15-full-ebf0740
python3 scripts/benchmark_report.py \
  $(find benchmarks/results/local-macos-2026-09-15-full-ebf0740 -name '*.txt' \
      ! -name 'commands.txt' ! -name 'environment.txt' ! -name 'progress.txt' \
      ! -name 'campaign.log' ! -name 'runner.status' | sort) \
  --json benchmarks/results/local-macos-2026-09-15-full-ebf0740/results.json \
  --markdown benchmarks/results/local-macos-2026-09-15-full-ebf0740/results.md \
  --environment benchmarks/results/local-macos-2026-09-15-full-ebf0740/environment.txt
```
