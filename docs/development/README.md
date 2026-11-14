# Development

**Descriptive** · repository build, tests, benchmarks and CI gates

How to build, test, benchmark and fuzz GlifiStore from a checkout, and what the automated benchmark
and portability workflows record. macOS and Xcode specifics are in [macOS and Xcode](macos.md).

## Build and test

The project uses CMake 3.25+, C++23, strict warnings, CTest, sanitizer presets and standalone fuzz
and benchmark targets.

`./scripts/dev.sh` is the single developer entry point on every platform. It selects the `macos-*`
presets on Darwin and the `unix-*` presets elsewhere (override with
`GLIFISTORE_PRESET_FAMILY=macos|unix`), prefers the repository-local `.tools/venv` and falls back to
`cmake`, `ctest` and `python3` on `PATH`:

```bash
./scripts/dev.sh doctor            # tools, preset family, checkout hazards
./scripts/dev.sh test              # configure, build, CTest
./scripts/dev.sh test -R blob      # extra arguments go to CTest
./scripts/dev.sh build --target glifistore_tests
./scripts/dev.sh help              # every command
```

The root `Makefile` mirrors the same commands (`make test`, `make verify`, …). The equivalent raw
CMake commands remain supported:

```bash
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --preset unix-debug
```

Keep the checkout outside iCloud- or Dropbox-synced folders: sync conflict copies such as
`store 2.cpp` and `.git/index 2` appear silently. `doctor` reports both.

## Before review

Run formatting, `./scripts/dev.sh verify`, unit and integration tests, the sanitizer appropriate to
the change, and any focused benchmark. Besides assurance, formatting, shell syntax, packaging policy
and tooling tests, `verify` checks the local links of all tracked Markdown. Build directories and
generated Xcode projects are never committed.

For a full local correctness campaign, run `./scripts/dev.sh correctness`. It executes the complete
Debug, strict, standard-library-hardening, ASan+UBSan and TSan matrices plus both production-only
clang-tidy layers. GCC, `scan-build`, standalone leak detection and libFuzzer are recorded as
`OPTIONAL_UNAVAILABLE` when the host cannot provide them; an available tool that fails remains a
hard failure. The machine-readable summary is written under `build/correctness/`.

## Automated benchmark reports

`.github/workflows/benchmarks.yml` runs the fixed Release benchmark suite after pushes to `main`, on
a weekly schedule and by manual dispatch. It records the runner environment and uploads the raw
benchmark output, `results.json` and the rendered `summary.md` for 90 days. The same summary is
shown on the GitHub Actions run page.

The push trigger is limited to the engine, server, public headers, benchmark sources, CMake
configuration, the canonical version and the report generator, so unrelated changes do not spend
hosted-runner time. Documentation, artwork and non-C++ SDK-only changes rely on their focused CI;
the weekly and manual full-benchmark safety nets remain unconditional.

The TCP portion is a scalability matrix with 1, 2 and 4 owner-bound clients and Workers and pipeline
depths 1, 8, 32 and 128. A separate `--latency` run reports p50, p95, p99 and p99.9 pipelined
response latency; keeping latency instrumentation separate avoids charging a clock read to every
response in throughput measurements.

Hosted-runner measurements are informational, because runner allocation, contention and CPU models
can change. Use a controlled, thermally stable runner before turning changes into hard regression
gates or publishing absolute throughput claims.

### Baseline comparison

The summary includes throughput deltas against the latest successful retained run on `main` only
when the machine-readable runner identity matches. Changes of CPU, runner image, kernel, compiler,
architecture, logical or physical CPU count, or build preset suppress the deltas and are listed in
the report rather than mislabeled as code regressions. The identity includes the SHA-256 digest of
the hosted matrix contract; individual matches include operation count, warmup and measured repeats.

For compatible environments, the report classifies overlapping min/max throughput ranges as
inconclusive; only disjoint ranges become improvement or regression candidates. When the retained
baseline and the current sample use the same Git revision, the report keeps the numeric delta but
classifies it as repeat variance: it cannot label same-revision noise as a code regression or
improvement candidate, even when the observed ranges are disjoint.

### Scaling signals

The JSON and Markdown reports derive, for each 1-, 2- and 4-Worker row, the highest observed median
pipeline, its gain over pipeline depth 1, its speedup against the one-Worker cell at the same depth
and its scaling efficiency. These are descriptive scheduling signals, not capacity claims.

Each TCP sample declares that it is same-process loopback and records the minimum timed
foreground-thread demand: the configured client threads plus one Reactor and one Writer per Worker.
The report compares that demand with the runner's logical CPUs, and the Worker count with physical
cores. Rows that exceed either capacity are labeled oversubscription sensitivity measurements, so
their efficiency is not misreported as physical-core server scaling evidence.

The report also selects the smallest measured pipeline whose median throughput is at least 95% of
the best median in the same Worker row. This economical pipeline is a reproducible near-peak choice
that limits in-flight work; it is advisory and does not override an application's latency or memory
requirements.

For every TCP cell the server benchmark also records the median and worst observed Reactor input and
output buffer compactions and bytes moved. The scaling summary normalizes median bytes moved by the
timed operation count, which distinguishes useful pipeline scaling from throughput that hides
growing copy pressure.

### Report validation

The workflow invokes the report parser in strict mode. Empty suites, duplicate identities, missing
metadata, count mismatches, invalid numbers, inconsistent statistical ordering, or disagreement
between a TCP filename and its runtime coordinates fail the report.

`engineering/performance/hosted-benchmark-contract.json` enumerates all 21 expected core, parallel,
durable, TCP-scaling and TCP-latency source files; missing or unexpected files invalidate the report
before baseline comparison. The same contract fixes one warmup and the expected three, five or seven
measured repeats for each source, preventing silent sampling cuts.

The durable-pipeline subsection requires both an explicit `durable-*` storage mode and completed
durable mutations; volatile server counters are never relabeled as durability measurements.

The parser also recognizes first-party specialized CSV/TSV outputs from compaction, maintenance,
generation publication, paired shard/Reactor and memory-census runs. These appear in a separate
direction-aware diagnostics table, must carry the same common metadata and complete repeat set, and
are not folded silently into the canonical ops/s regression threshold.

### Run the report parser locally

The parser also accepts one or more local benchmark output files:

```bash
python3 scripts/benchmark_report.py benchmark-results/*.txt \
  --json benchmark-results/results.json \
  --markdown benchmark-results/summary.md
```

Add `--strict` when the local outputs are intended to exercise the CI evidence contract. Pass
`--source-contract engineering/performance/hosted-benchmark-contract.json` with the complete hosted
matrix to enforce its exact source set.

## Durable TCP audit matrix

The durable TCP audit matrix runs the v1 sync, strict group and periodic policies at pipeline depths
1, 8 and 32. It includes both 1/2/4 client-to-Worker scaling and four clients per Worker at each
scale, because strict group commit cannot form multi-record batches with only one outstanding
mutation producer per Worker. It records raw output, exact queue, Store and batch metrics,
environment metadata, JSON and Markdown below an ignored timestamped directory:

```bash
./scripts/benchmark_durable_server.sh
# Override duration without changing the matrix contract:
BENCHMARK_OPS=1000 BENCHMARK_REPEATS=5 ./scripts/benchmark_durable_server.sh
```

Throughput and latency runs are separate so steady-clock sampling does not contaminate the
throughput baseline. `durable-periodic` measures its relaxed acknowledgement contract; its later
store-wide sync must not be compared as though it were included in response latency. Unbatched
`durable-sync` reports zero for the batch-commit metric; its complete filesystem cost remains
included in Store service time.

## Other benchmarks

Local maintenance and durable matrices write under the gitignored `benchmark-results*/` through
`./scripts/dev.sh benchmark`, `benchmark-durable` and related harness scripts. Do not commit raw run
outputs. Comparative GET and hot-cache notes live under `docs/benchmarks/`, methodology and medians
only; reproduce them with `./scripts/benchmark_get_path.sh` or the focused filters in those notes.

`glifistore_server_benchmarks` also has an opt-in controlled maintenance-overlap profile. Set
`--maintenance-overlap-seed-operations` to prebuild a reclaimable multi-Segment dataset on the last
Worker; owner-bound client 0 then drives the first Worker. The benchmark holds the initial
background space probe until `--maintenance-overlap-release-ms` after foreground start, so
compaction admission cannot race ahead of latency sampling. Seed key/value size and evaluation
cadence are controlled by `--maintenance-overlap-seed-keys`,
`--maintenance-overlap-seed-value-bytes` and `--maintenance-overlap-eval-ms`. Run alternating
threshold-off/on pairs; setup is outside the timed region, but every repeat creates and recovers a
fresh Store.

`./scripts/benchmark_tls_tax.sh` measures the TLS 1.3 tax over cleartext on the Go pipeline harness;
see the [TLS performance note](../security/tls-performance.md).

## Portability gates

The OpenBSD/LibreSSL correctness gate is not a throughput benchmark. Run
`bash scripts/ci-openbsd-libressl.sh` on OpenBSD, or the
[OpenBSD LibreSSL workflow](../../.github/workflows/openbsd-libressl.yml). The workflow partitions
the registered `glifistore_tests` cases by a stable test-name hash across two independent VM jobs;
their union is the complete suite. Both jobs retain native logs, and one also performs the Go TLS
PUT→GET smoke. A native developer run remains unsharded unless `GLIFISTORE_TEST_SHARD_COUNT` and the
zero-based `GLIFISTORE_TEST_SHARD_INDEX` are both set. The CI invocation streams each `[RUN]` marker
and limits every CTest target to 15 minutes, so a stalled case fails with an attributable name
instead of consuming the two-hour VM budget.

The FreeBSD native build and test gate is a portability signal, not UFS or ZFS durability
certification. Run `bash scripts/ci-freebsd.sh` on FreeBSD, or the
[FreeBSD workflow](../../.github/workflows/freebsd.yml).

## Fuzzing

Continuous fuzz smoke uses Linux Clang libFuzzer:

```bash
cmake --preset unix-fuzz
cmake --build --preset unix-fuzz
GLIFISTORE_FUZZ_SECONDS=30 ./scripts/run-fuzzers.sh
```

CI runs each target in `.github/workflows/sanitizers.yml` (`fuzz-run`) against
`fuzz/corpus/<target>/`: 60 s on pull requests and pushes, 120 s on the Monday schedule and manual
dispatch.
