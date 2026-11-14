# Test strategy

**Normative** · all supported builds · reviewed 2026-08-30

The quality policy for tests: what each layer of evidence proves, what each kind of change must
cover, which CI tier runs what, and the commands that reproduce it locally.

## Principles

Tests provide layered evidence. Unit tests prove local contracts, integration tests prove component
composition, property tests explore histories, crash tests prove recovery boundaries, sanitizers
expose classes of undefined behavior, and compatibility fixtures freeze bytes. No single layer
substitutes for another.

All tests must be deterministic by default. Randomized tests record their seed on failure. Tests
must not depend on directory iteration order, wall-clock races, internet access or developer-machine
state.

## Suites

| Suite | Purpose | Typical location |
| --- | --- | --- |
| Unit | Codec, algorithm, bounds, queue, policy | `tests/unit` |
| Integration | Store, runtime, network and recovery composition | `tests/integration` |
| Property/history | Model agreement over operation sequences | `tests/property` |
| Crash | Kill/reopen durability checkpoints | `tests/crash` |
| Quality/fault | Allocation failure and fail-closed behavior | `tests/quality` |
| Consumer | Installed-package and public-header usability | `tests/consumer` |
| Fuzz | Untrusted decoder state space | `fuzz` |

## Mandatory contract coverage

| Change | Must cover |
| --- | --- |
| Index | Empty and binary keys, collisions, wraparound, deleted chains, resize, arena boundaries, allocation failure, scalar/SIMD equivalence |
| Persistent format | Exact golden bytes, round trip, every truncated length class, overflowed lengths, bit flips, unknown versions, nonzero reserved bytes, checksum scope, allocation limits, encode canonicality |
| Recovery | Missing, extra and aliased objects; both commit slots; crash tail versus committed corruption; sequence conflicts; interrupted bootstrap, rotation and compaction; resource preflight; enumeration-order independence |
| Concurrency | Same-key and different-Worker histories, close races, flush waiters, queue full/empty/wraparound, ownership transfer, sanitizer runs; deterministic orchestration is preferred to sleeps |
| Protocol | Exact headers, partial frames, multiple pipelined frames, maximum boundaries, invalid opcode/version/size, session order, wrong owner, output backpressure, EOF, handoff saturation |

## Sanitizers and tools

- ASan/UBSan: memory safety, alignment, overflow paths not already checked.
- TSan: data races in Store, coordinator and server tests.
- Fuzzing: Record, Manifest, Segment header, intent and protocol decoders.
- Warnings-as-errors and formatting: every supported compiler profile. A compile-database pass over
  production sources treats unchecked optional access, use-after-move, analyzer dead stores,
  declaration/definition parameter drift, unbraced bodies, duplicate includes, obsolete null
  constants and missing `override` as hard failures. The [native code style](code-style.md) fixes
  the exact formatter version and shares one complete source inventory between local checks and CI.
  The broader all-target clang-tidy build remains visible for triage without conflating known
  test-macro/toolchain noise with a closed gate.
- Documentation: every tracked Markdown file is UTF-8 and its repository-local links resolve with
  exact path case; network-dependent links remain a separate scheduled check.

A sanitizer exclusion needs a documented toolchain defect or unsupported primitive and a
compensating test.

## Crash evidence

Inject failure before and after every durability ordering boundary. Each checkpoint must specify the
expected recovered namespace and whether the last mutation is required, optional or forbidden.
Process termination verifies software ordering; platform power-loss testing is separately required
for durable certification.

Crash tests retain failing directories or a reproducible artifact description. Recovery must produce
the same outcome regardless of file enumeration order.

Native evidence uses the levels, row metadata, promotion rules, E2 collector and E3 block-reset
harness in the
[platform durability evidence matrix](../architecture/platform-durability-evidence.md). A
hosted-runner pass without a pinned filesystem row must not be reported as power-loss or release
certification. Harness smoke (`linux-ext4` loopback, `macos-apfs` diskimage) is rehearsal only. The
E3 controller requires a confirmed paused worker before reset; Linux dm-flakey rehearsals classify
`drop-writes`, `error-writes` and `all-io-error` separately in their artifacts.

## CI tiers

| Tier | Runs |
| --- | --- |
| 1. Per change | Build, unit/integration/property tests, formatting; ARM64 Linux matrix; release + LTO smoke; SDK clients; assurance validators; actionlint on workflow diffs; dependency-review; CodeQL (C/C++, Python, Go, Actions); Scorecard on `main` |
| 2. Required extended | ASan/UBSan and TSan |
| 3. Nightly / scheduled | Continuous fuzz smoke (Sanitizers `fuzz-run`, 120 s per target on Monday), crash matrix, opt-in exhaustive and randomized compaction matrices, broader compilers and platforms including FreeBSD and OpenBSD VM gates; a weekly 256-case reproducible randomized crash/reopen campaign; docs link check; license hygiene; diagnostic coverage |
| 4. Release | Full fixtures, installed consumer, benchmarks, long fuzzing, durable platform evidence, tag supply-chain (SBOM/Cosign/attest) and GitHub Release artifact attach |

Relevant persistence and paired-runtime changes also run the bounded TLA+ matrix in
`.github/workflows/formal-models.yml`. Both the shard pair liveness model and the
persistence/recovery safety model are required: a TLC error or wall-clock timeout fails the job. The
persistence model is an abstract logical proof and does not raise E2 evidence to physical E3/E4.

Benchmark smoke tests validate harness correctness; they are not performance gates. Performance
regression gates require stable runners and a historical variance policy.

Branch-protection and required-check settings are in
[GitHub branch protection](../assurance/github-branch-protection.md).

## Coverage policy

Line coverage, including its CI artifacts, is diagnostic, not the acceptance metric. Review must map
each normative invariant and failure branch to evidence. A change is under-tested when its new state
transition, ownership transfer or recovery ambiguity has no test, even if aggregate coverage rises.

## Commands

```sh
./scripts/dev.sh test
./scripts/dev.sh verify
./scripts/dev.sh asan
./scripts/dev.sh tsan
./scripts/dev.sh fuzz-build
./scripts/dev.sh fuzz-run
./scripts/dev.sh test-lto
```

Run focused binaries while iterating, then the repository workflow appropriate to the change before
handoff.

`fuzz-run` expects a prior `fuzz-build` (or an equivalent `unix-fuzz` or `macos-fuzz` build) and
defaults to 60 s per target through `scripts/run-fuzzers.sh`. Override it with
`GLIFISTORE_FUZZ_SECONDS`.

### Deprecated `legacy_mutex` mode

The deprecated `legacy_mutex` open mode is exercised inside `glifistore_tests` by white-box and
concurrency cases that set `StoreConfig::concurrency = StoreConcurrencyMode::legacy_mutex`
(`tests/integration/concurrency_tests.cpp`, `optimization_regression_tests.cpp` and selected
`store_tests.cpp` cases):

```sh
ctest --preset macos-debug -R '^glifistore_tests$' --output-on-failure
```

On Linux CI presets use the matching configure preset (`unix-debug`, `unix-asan`, …). No separate
legacy-only binary is required; those tests open Stores with the deprecated flag explicitly.

### OpenBSD sharding

The OpenBSD correctness workflow runs the same registered cases in two deterministic shards to keep
the native virtualization gate inside its time budget. Sharding is fail-closed: count and zero-based
index must be supplied together, malformed or empty configurations are rejected, and a stable hash
of each test name partitions the registry without overlap or omission across independently built CI
jobs. Direct executions remain unsharded by default. The native gate uses verbose CTest output and a
15-minute target timeout; the harness flushes a `[RUN]` marker before every case so a stall remains
attributable in the retained log.

### Formal models

Run both formal models with a locally supplied `tla2tools.jar`:

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar engineering/formal/shard_pair/run-tlc.sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar engineering/formal/persistence/run-tlc.sh
```

### Crash matrices

Run the long multi-output profile separately. Together with `glifistore_crash_sync`, it covers all
152 persistence checkpoints and every one of the 64 Record copies:

```sh
build/macos-debug/glifistore_crash_persistence --mode copy-matrix
build/macos-debug/glifistore_crash_persistence --mode random-matrix
build/macos-debug/glifistore_crash_persistence \
  --mode random-campaign --campaign-seed 42 --iterations 1000 \
  --report /path/to/new-report.tsv
```

### Durability evidence

To retain attributable E2 evidence from a configured build:

```sh
scripts/collect-durability-evidence.sh \
  --output /path/to/new/evidence-directory \
  --build-dir build/macos-debug \
  --probe-path . \
  --run process-kill
```

For a checksummed E2 randomized artifact, with commit, toolchain and filesystem provenance, the
exact seed, structured per-reopen outcomes, logs and a SHA-256 manifest:

```sh
scripts/collect-durability-evidence.sh \
  --output /path/to/new/evidence-directory \
  --build-dir build/macos-debug \
  --probe-path . \
  --run random-campaign \
  --campaign-seed 42 \
  --iterations 1000
```

The harness accepts at most 10,000 cases. GitHub-hosted dispatches cap the value at 512 and the
weekly profile uses 256; larger campaigns belong on a dedicated runner. This remains E2 evidence,
not block-reset or physical E3/E4 evidence.

To rehearse an E3 block-reset on a disposable pinned row (still `e3_certified=no`):

```sh
scripts/run-e3-block-reset.sh \
  --output /path/to/new/e3-artifact \
  --build-dir build/unix-debug \
  --platform linux-ext4 \
  --profile smoke \
  --reset-mechanism dm-flakey \
  --dm-fault-mode drop-writes
```

For an operator campaign-prep package (E0→E1→E2→E3, many repetitions, an evidence tarball and a
SHA-256 manifest; still `e3_certified=no` until human promotion), see the
[E3 pinned-row campaign](../operations/e3-campaign.md) and `scripts/run-e3-campaign.sh`.
