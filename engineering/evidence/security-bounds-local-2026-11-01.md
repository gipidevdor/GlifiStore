# Local network-security bounds evidence — 2026-11-01

Evidence class: **local**. Base: `eded7513bfef2af5e3901b8fff08cc21d2af63ab` plus this
uncommitted remediation. Host: macOS arm64, AppleClang 21, OpenSSL 3, C++23/CMake/Ninja.
This record does not certify production readiness, hostile-public exposure or physical durability.

## Contract

[ADR 0041](../../docs/adr/0041-bounded-network-security-lifecycles.md), GS-SEC-BOUNDS-001,
GS-SEC-PROFILE-001 and HAZ-024 govern the corrected TLS/output/admission/audit lifecycles.
Persistence v1, wire v2, Store acknowledgement points and indeterminate-on-disconnect remain.

## Baseline and verification

Before production changes, focused regressions failed for oversized response charging,
denied-request/probe admission, stalled TLS alongside an established session, and pending TLS
slot accounting. A normal TLS handshake/PING control passed. The initial slow-output test was
inconclusive and was strengthened to require a decided response, output-expiry counter, scatter
path participation and reuse of the freed connection slot.

## Initial bounds verification results

| Gate | Command / scope | Result |
| --- | --- | --- |
| Build | CMake Debug, C++23, TLS ON, fault injection ON, warnings as errors; glifistore_tests and glifistored | Passed |
| Security triggers | glifistore_tests 'security bounds' | 4/4 passed, including continued-input denial drain |
| Original TLS failure classes | glifistore_tests 'tls' | 15/15 passed, including stalled handshake, slot/deadline/stop and normal mTLS/CRL/client controls |
| Output regression sensitivity | Disposable object/library copy with output-expiry branch removed | Expected failure at slot-release assertion; final candidate passes |
| Nearest controls | abuse controller, security audit, timeout, rate limit, half-close and handoff filters | Passed after the explicit ADR 0041 drain-contract test adjustment |
| Initial native suite | glifistore_tests | **741/741 passed** after the receive-stop correction |
| TLS OFF | Separate strict Debug build; security bounds and tls filters | 4/4 and 6/6 passed |
| Memory / UB | ASan+UBSan strict Debug TLS build; tls, security bounds, abuse controller, security audit, handoff, half-close and late cold-read completion filters | **37 selected executions passed**, overlapping filters; no sanitizer report |
| ABI / daemon | Build glifistored and glifistore_c_abi_tests; CTest glifistore_c_abi_tests and glifistore_abi_symbols | 2/2 passed; C ABI exports unchanged |
| Assurance | validate_assurance.py, including regenerated views | Passed: 36 requirements, 34 hazards, 30 gates, 3 waivers; no warnings |
| Repository quality | validate_documentation.py, validate_cmake_deps.py, validate_structure_debt.py, validate_durability_claims.py, validate_claims.py; changed-source clang-format dry run; git diff --check | Passed |

Native full-suite controls include session framing, paired Store mutation visibility, late
completion/slot reuse, handoff, half-close, durable recovery and client error semantics. The
original malformed accounting/admission/TLS triggers no longer reproduce in the final candidate.
The output test's mutation check demonstrates that it depends on the new expiry control rather
than an unrelated close. Its initial mutant run exceeded the harness timeout while unwinding the
old unbounded drain; the test cleanup was bounded to 200 ms and the repeat produced the intended
assertion failure. No mutant source or library was applied to the checkout.

The read-only candidate reviewer identified a close_after_flush receive loop that could keep
discarding new input before timeout enforcement. The final patch stops application reads on that
state, preserves TLS control progress required for SSL_write, and adds a concurrent junk-input /
healthy-probe / expiry regression. The independent review was interrupted by an automated tool
risk filter after that finding; the parent completed the remaining caller/state/platform review.
No independent all-clear is claimed.

### Reproducible command families

All build outputs were created in the plugin-prepared disposable workspace, not the repository.
Use a fresh BUILD directory and the repository's CMake/Ninja executables or equivalents:

```sh
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DGLIFISTORE_BUILD_BENCHMARKS=OFF \
  -DGLIFISTORE_ENABLE_TLS=ON -DGLIFISTORE_FAULT_INJECTION=ON \
  -DGLIFISTORE_WARNINGS_AS_ERRORS=ON -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build "$BUILD" --target glifistore_tests glifistored glifistore_c_abi_tests -j 4
"$BUILD/glifistore_tests"
ctest --test-dir "$BUILD" -R '^(glifistore_c_abi_tests|glifistore_abi_symbols)$' --output-on-failure
```

The TLS-off build sets GLIFISTORE_ENABLE_TLS=OFF. The sanitizer build adds
GLIFISTORE_ENABLE_ASAN=ON and GLIFISTORE_ENABLE_UBSAN=ON, with ASAN_OPTIONS=halt_on_error=1:abort_on_error=1
and UBSAN_OPTIONS=halt_on_error=1. It runs only the listed focused filters; a complete sanitizer
suite, leak proof, TSan campaign and fuzz campaign were not run. The later hosted platform
matrix is recorded in the [CI campaign](security-bounds-ci-2026-11-01.md).
Raw baseline, final native, TLS-off, sanitizer and mutation logs are retained in the chat's
managed remediation artifact collection, separately from the sealed original scan.

GATE-AUTH-TRANSPORT was initially reopened to PROVATA_LOCALMENTE because old CI evidence could
not prove the amended lifecycle. The later [CI campaign](security-bounds-ci-2026-11-01.md)
passed on bc0b41e and restores PROVATA_IN_CI for the documented platform rows, without release
acceptance or production/public-readiness claims.

## Audit sink isolation follow-up

The follow-up replaces synchronous Reader-side emission with ADR 0041's single process-lifetime
emitter and fixed queue. This supersedes the synchronous-stderr residual from the initial campaign.
The queue retains owned lines and shared output counters, never Server/Audit pointers. An emitter
blocked in inherited stderr is never joined by Server destruction. No descriptor is changed to
O_NONBLOCK. On Darwin F_SETNOSIGPIPE is required because pipe EPIPE signals are process-directed;
the shared no-SIGPIPE attribute is explicitly documented in the ADR.

| Proof | Result |
| --- | --- |
| Strict Debug TLS ON build, server/daemon/test targets | Passed |
| Full native suite on source commit 67ad42f8ef8c6750598cb5436d8b2dedd164c5ce | **740/740 passed**, plus all four sink subprocesses; one old synchronous-output assertion moved into the dedicated executable |
| Final sink scenarios: normal JSON/quiet/escaping/storm, full blocking pipe, closed pipe, nonblocking backpressure | **4/4 passed** with 15-second CTest deadlines |
| Same final scenarios with TLS OFF | **4/4 passed** |
| Same final scenarios with ASan+UBSan | **4/4 passed**, no sanitizer report |
| Full-pipe security trigger | Audit calls return, single-Reader HEALTH succeeds after denied PUT, exact denial counters remain available, join and actual Server destruction finish while the pipe stays full |
| Lifetime / flags / signals | Destroyed Audit counter state survives queued emission; O_NONBLOCK/status flags stay unchanged; closed-pipe test preserves default SIGPIPE and survives |
| Regression sensitivity | Disposable copy replacing enqueue with synchronous sink write blocks on full pipe and is killed after 3 seconds; checkout unchanged |
| Independent read-only review | No surviving Reader-blocking route found; partial-write/error hypothesis confirmed by source inspection and addressed by retaining suffixes on EAGAIN and disabling the sink on hard error |

The final error-handling refinement was followed by all four sink scenarios in the three local
build variants and the complete 740-test native suite. The final naming-only build correction also
passes a syntax check with -Wshadow-all -Werror and the native suite. Hosted CI must execute the
final committed sources. Raw CTest logs and the mutation result are retained in the separate managed
remediation collection under `artifacts/fixes/audit-isolation/`. OpenBSD's explicit build-target
and CTest filter now include the new executable; Linux/FreeBSD already build and test all targets.

## Residual risk

- A blocked stderr can retain one process-lifetime emitter and the fixed queue. Audit is best
  effort: later events are suppressed; pending events may be lost at process exit. Hard I/O errors
  disable the sink; durable audit, sink rotation and post-initialization fork are unsupported.
- One already-decided response may exceed the residual byte allowance; this is admission
  accounting, not packet-level egress shaping. Both timeout knobs zero disables output expiry.
- The local macOS host covers kqueue only. Linux epoll, FreeBSD and OpenBSD/LibreSSL results
  are retained separately in the [hosted campaign](security-bounds-ci-2026-11-01.md), which
  proves the same final implementation and records the two test-portability corrections.
