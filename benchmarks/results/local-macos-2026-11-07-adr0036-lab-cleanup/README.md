# ADR 0036 lab cleanup evidence — 2026-11-07

Status: local measurement; architectural prototype
Source commit: `8ff89d3fefa6119a273d8f8ba7fe7d345bcee042` (Phase 2, tracked tree clean when built and measured)
Host: Apple M4, 10 physical / 10 logical CPUs,
16 GiB RAM; Darwin 27.2.0, arm64; AC power.
Build: `unix-release`, `GLIFISTORE_BUILD_LAB=ON`, Apple LLVM 21.0.0.

## Method and provenance

All rows use the same machine, compiler and Release build. Every implementation/mode has one
warmup and seven measured repeats; repeat sets 0..6 and nonzero checksums were checked. The
production checksums equal the exact requested cycle count. Raw outputs include their compiled
source SHA, compiler, architecture and warmup/repeat metadata. `metadata.json` retains commands,
UTC timestamps, CPU/memory/power details, load averages and binary SHA-256 hashes.

The shell and publication diagnostics each use 20,000 publications. Publication is run both
with adoption-only Reader work and with L1-hot GET work. Affinity is reported **unavailable** on
this macOS host; these are not hard-pinned Linux rows.

The production driver uses the **official embedded paired Store**, not the candidate pool or a
second runtime. It sets `PairedConcurrencyConfig::generation_slot_pool` to false and true in the
same binary, uses 64 keys per worker, 64-byte values, volatile storage and one caller per owner.
Each caller performs 200,000 cycles. A cycle is one PUT, one owning GET, or PUT followed by its
owning GET, according to the workload. Setup/seeding and final validation/close are outside the
timed window; caller startup/join overhead is included. A/B and B/A order alternates across
repeats after warming each mode. Per-thread counters are on separate 128-byte alignments.

Tables report medians of throughput; percentile columns are medians of per-run sampled
percentiles. `summary.json` also retains min/max and inclusive quartiles. This host was not
isolated: the before/after 1-minute load averages were 4.99 /
6.66. The result is local evidence, not a capacity guarantee,
statistical confidence interval, multi-OS proof, durability certification or release claim.

## Shell diagnostic

| Shell implementation | Publications/s | Delta vs make_shared |
| --- | ---: | ---: |
| make_shared | 2,614,536 | +0.00% |
| fixed_shell | 2,596,124 | -0.70% |
| owning_pool | 2,426,583 | -7.19% |
| inline_pool | 2,569,979 | -1.70% |
| direct_ring | 2,577,140 | -1.43% |
| direct_slot_pool | 2,483,033 | -5.03% |

## Concurrent publication diagnostic

| Reader work | Shared-slot pub/s | Direct-slot pub/s | Throughput delta | Sampled publication p99 shared / direct (ns) | Reader GET p99 shared / direct (ns) |
| --- | ---: | ---: | ---: | ---: | ---: |
| adopt | 1,143,519 | 1,174,674 | +2.72% | 1,458 / 1,542 | 0 / 0 |
| get | 865,692 | 931,999 | +7.66% | 1,750 / 1,750 | 292 / 250 |

The sampled publication p99 in adoption-only mode rises from 1,458 to 1,542 ns (+5.76%).
The GET mode keeps publication p99 at 1,750 ns and lowers Reader GET p99 from 292 to 250 ns.
These isolated protocols omit the production Store operation and cannot replace its A/B row.

## Official production path

| Workload | Threads / workers | Flag false, cycles/s | Flag true, cycles/s | Throughput delta |
| --- | ---: | ---: | ---: | ---: |
| PUT | 1 | 671,147 | 584,365 | -12.93% |
| GET | 1 | 7,388,742 | 7,342,750 | -0.62% |
| PUT-GET | 1 | 600,153 | 522,369 | -12.96% |
| PUT | 2 | 810,255 | 756,022 | -6.69% |
| GET | 2 | 13,212,330 | 13,468,864 | +1.94% |
| PUT-GET | 2 | 789,648 | 735,918 | -6.80% |

For PUT 2t, false spans 789,762–839,172 cycles/s, true 727,815–794,093 cycles/s. The measured
median ratio is 756,021.53 / 810,254.79 - 1 = -6.69336%. These are throughput deltas;
negative means slower. No conclusion is drawn by comparing rates across different workloads.

## Recommendation

**Recommendation: reject promotion of the current ADR 0036 implementation.** The official
worker-affine PUT 2t median regresses **6.69%**, crossing V12's hard reject threshold of more
than 5%. PUT 1t also regresses 12.93%, and PUT→GET regresses 12.96% / 6.80% at 1t / 2t.
GET alone changes -0.62% / +1.94%; its ranges overlap and it does not offset the mutation
regression. Lab publication gains therefore do not establish a gain in the official Store path.
Keep Alternative A as the default and leave the production opt-in flag unchanged. This is a
recommendation for this source/host row, not a decision by the agent: **Status stays proposed**
and acceptance/rejection remains the maintainer's responsibility.

V11 gains a local official-runtime A/B row; V12 rejects this measured implementation for
promotion. This does not close the remaining proof rows, including the full opt-in durability,
crash, sanitizer and platform matrix. No production code, flag default, ACK point, disk/wire
format, routing or borrow lifetime was changed for the campaign. Claim ceiling unchanged.

## Cleanup verification before the campaign

- Phase 0: grep found zero real official includes of `experimental/`; only the expected comment.
- Phase 1 (`e07d637`): `dev.sh verify` passed (435 tooling tests); configure/build `unix-debug`
  passed; CTest 72/72 passed. The nine removed files are not kept in the repository.
- Phase 2 (`8ff89d3`): `dev.sh verify` passed (435 tooling tests); configure/build `unix-debug`
  passed; CTest 70/70 passed with lab OFF. A separate Debug lab ON build passed all 26 candidate
  cases (one CTest suite), including the four relocated integration cases.
- CMake compile commands: lab OFF has zero experimental translation units. Lab ON compiles the
  bridge only in lab targets. Neither configuration compiles experimental files into
  `glifistore_tests`; the official include prohibition is retained.
- Structure gate: the official reactor test file is 881 lines and the relocated lab file 316,
  both below 1,600. The four remaining experimental source files are unchanged from the tag.

## Reproduce into a new directory

From the repository root, with the same supported C++23 toolchain:

```bash
cmake --preset unix-release -DGLIFISTORE_BUILD_LAB=ON
cmake --build --preset unix-release --target glifistore_generation_shell_benchmark \
  glifistore_generation_publication_benchmark glifistore_generation_runtime_benchmark
python3 benchmarks/results/local-macos-2026-11-07-adr0036-lab-cleanup/collect.py \
  --output benchmarks/results/local-macos-NEW-RUN-adr0036-lab
```

The collector refuses to overwrite an existing campaign directory and requires a clean tracked
tree. The exact per-command arguments are in `metadata.json`; raw TSV files are the evidence,
while the JSON summary is a derived view.
