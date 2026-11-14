# Local quality-hardening evidence — 2026-08-30

Evidence class: **local**. This record does not promote a gate to `PROVATA_IN_CI`, certify a
platform durability row, accept ADR 0036, or raise the architectural-prototype claim ceiling.

## Environment

- Base revision: `5fa79cd17f77271c0c109ddec91faf4eda55ea70` plus the uncommitted follow-up changes
  described here.
- Host: macOS 26.6.2, Apple clang 21.0.0, arm64.
- Formatting: clang-format 21.1.8.
- Static analysis: clang-tidy 21.1.6 for the initial campaign; clang-tidy 22.1.8 for the
  post-CI follow-up.

## Scope

- Normalize the tracked C++ tree to the pinned formatting policy.
- Validate UTF-8 and exact-case repository-local links across every tracked Markdown document.
- Add a compile-database-derived, production-only fail-closed clang-tidy pass.
- Harden optional-state, move-ownership, declaration consistency, dead-store, and fail-closed
  paths reported by that pass.
- Preserve ADR 0036's proposed/default-off status while adding the V7 slot-pressure evidence and
  terminal shutdown cleanup already described in the ADR-specific local record.

## Results

| Check | Local result |
| --- | --- |
| Assurance validation | 33 requirements, 33 hazards, 28 gates, 3 waivers; 0 warnings |
| Documentation validation | 257 Markdown files; 1,035 repository-local links |
| Python/tooling tests | 104 passed |
| Pinned clang-format dry run | all `.cpp`/`.hpp` files in the CI-gated source roots passed |
| High-signal clang-tidy gate | 109 production sources; 4 fail-closed checks; passed |
| Broad clang-tidy build | completed; lower-confidence test/benchmark/toolchain diagnostics remain visible |
| `unix-strict` build | 235 build steps; passed with compiler warnings as errors |
| `unix-strict` tests | 54/54 passed; 351.02 s |
| macOS ASan+UBSan tests | 54/54 passed; 464.72 s |
| macOS TSan main suite after repair | 696/696 passed; no TSan report |
| Post-CI macOS Debug CTest | 54/54 passed after final analyzer-portable extraction; 326.72 s |
| Paired continuous-Reader adverse-scheduling repetition | 500/500 each for linearization and overwrite-storm litmus tests |

The first full TSan main-suite run exposed three reports with one root cause: the dedicated
volatile Writer read a caller-stack `SyncMutation::status` after publishing `done`. The Writer now
derives the all-failed/indeterminate decision before the release notification and never
dereferences those nodes afterward. Both direct reproducer tests and the complete 696-test TSan
main suite passed after the repair.

## Post-CI reconciliation

The first pushed campaign exposed two additional classes of quality debt without changing the
architectural-prototype claim ceiling:

- the Ubuntu clang-tidy package diagnosed 24 unchecked `std::optional` accesses across 10
  production sources that clang-tidy 21.1.6 had not reported locally. The first repair used an
  explicit `value()` extraction after each fail-closed member guard, but the older CI analyzer did
  not propagate that guard through the enclosing result/member access and retained 19 diagnostics.
  The final form first binds each member `optional` to a local reference, then guards and extracts
  through that same alias, avoiding an additional move in hot paths. The full 109-source gate
  passes locally with clang-tidy 22.1.8; a focused clang-tidy 18.1.8 data-flow probe accepts this
  local-reference pattern while reproducing the diagnostic on the original nested access;
- two macOS paired litmus tests failed in successive CI campaigns because their Writers treated
  the documented bounded generation-admission `resource_exhausted` result as a visibility or
  linearizability failure while a Reader held a nearly continuous sequence of counted leases. A
  shared test-only handshake now creates an explicit quiescent hand-off and retries the same
  mutation; GET errors, empty values, and any non-backpressure mutation error remain independent
  hard failures. Both tests pass 500 adverse-scheduling repetitions each; the first repair also
  passed the complete Debug CTest matrix;
- the packaged Erlang concurrency suite exposed a lost update in the fake server's ETS `held`
  list when two connection processes registered concurrently. Per-process ETS keys remove the
  read/modify/write race, while publishing `release_all` before collecting keys and rechecking it
  after registration closes the late-registration window.

These are static-analysis and proof-harness repairs. They do not alter persistence format v1, wire
protocol v2, acknowledgement points, routing, recovery, or public mutation visibility.

The focused eight-Worker/eight-client/pipeline-128 volatile read-after-write repeat also did not
reproduce the full-campaign performance outlier. Three interleaved seven-sample rounds produced a
current-tree median-of-medians of 301.19 kops/s and p99 of 7.29 ms, versus 287.44 kops/s and 7.45 ms
at `1ff35c3`. The wide per-round overlap prevents a performance-change claim, but rules out using
the isolated 195.72 kops/s / 35.21 ms cell as evidence of a deterministic code regression.

## Residuals

- The complete post-repair TSan CTest matrix was not rerun: its crash-sync row permits up to 2,400
  seconds. The complete main concurrency suite was rerun; strict and ASan+UBSan ran all 54 CTest
  rows, including crash/recovery.
- CI, Linux, FreeBSD, and OpenBSD results remain external to this local record.
- The broad all-target clang-tidy profile is diagnostic, not warning-free. Only the documented
  high-signal production pass is fail-closed.
- A complete clang-tidy 18.1.8 pass cannot be treated as local evidence on this host because that
  older frontend cannot parse the current macOS 26/Xcode libc++ headers. The Ubuntu CI row remains
  the authoritative proof for its packaged analyzer.
- External URL availability and Markdown fragment semantics remain covered by the separate
  scheduled link workflow, not the deterministic local-link validator.
- Erlang/OTP is not installed on this macOS host, so the repaired Erlang suite awaits CI execution;
  this local record does not count that row as passed.
