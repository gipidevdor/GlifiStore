# Local C++ correctness repair campaign — 2026-11-07

Evidence class: **local**. GlifiStore remains an architectural prototype. This record does not
promote a gate, accept ADR 0036, certify durability or claim cross-platform sanitizer coverage.

## Subject

Base revision: `62d87cda086f06b4a11bfaf197cad13bae9ad2ae` plus this uncommitted repair.
Concurrent Dependabot/workflow and large-blob design edits are outside the C++ repair subject.
The [machine-readable summary](cpp-correctness-2026-11-07/summary.json) retains the exact SHA-256
of every C++/CMake subject file and the logs below.

The repair restores five contracts:

1. C++ PUT/ERASE preserve observed send progress through allocation failures, reset the connection,
   and return `indeterminate` after positive bytes. Fallback error strings are prepared before I/O;
   BACKUP and reads use the same protected exchange boundary.
2. Expired deadlines reject immediately writable/readable sockets and buffered pipeline responses;
   response materialization and final validation remain inside the absolute request budget.
3. Slot-pool observer construction and mirroring allocate no control block. The pool, leases and
   existing pins remain the lifetime authority; the flag stays optional/default-off.
4. OwnedBytes handles internal source subranges before releasing heap storage and uses overlap-safe
   copying for inline values.
5. PreparedColdRead constructs State through the raw storage pointer and launders only live objects.
   Move construction, replacement and reconstruction exercise the existing pin contract.

Authorities: [ADR 0019](../../docs/adr/0019-client-error-retry-timeout.md),
[client semantics](../../docs/spec/client-semantics-v1.md),
[ADR 0036](../../docs/adr/0036-generation-slot-pool-publish.md),
[public read ownership](../../docs/adr/0009-public-read-ownership.md).
Requirements: GS-PROTO-ERROR-001, GS-CORE-API-001, GS-CONCUR-PAIR-001.
Hazards: HAZ-005 (unsafe retry), HAZ-008 (invalid lifetime/use after free).

## Regression proofs

- [Transport deadlines](../../tests/unit/client_transport_tests.cpp): deterministic expired budgets
  with already writable sockets, already readable responses, and already buffered pipeline data.
- [Client allocation faults](../../tests/quality/allocation_fault_client_tests.cpp): per-thread
  allocation sweeps over PUT/ERASE against a real one-shard Server; applied mutations cannot be
  rejected; positive send count, request id, outcome and reconciliation survive; a subsequent read
  uses a valid connection. A 100 ms pause during response allocation exceeds a 50 ms budget and
  must return transport/indeterminate. Slot observer/mirror operations run with allocation forbidden.
- [Owned bytes](../../tests/unit/value_tests.cpp): varied byte patterns and aliasing subranges across
  inline/heap storage, including empty, exact-inline-boundary and heap-retaining assignments.
- [Cold read moves](../../tests/unit/prepared_read_tests.cpp): real durable reads retain the intended
  record across State moves, replacement, reconstruction and final completion.

## Results

| Check | Local result | Retained evidence |
| --- | --- | --- |
| Debug main suite | 746 tests, 0 failures | [Full test log](cpp-correctness-2026-11-07/debug-tests.log) |
| Debug allocation fault suite | Passed, including the three new campaign functions | [Fault log](cpp-correctness-2026-11-07/debug-allocation.log) |
| ASan + UBSan CTest selection | 5/5 targets passed; main suite 746 tests, 0 failures | [CTest summary](cpp-correctness-2026-11-07/asan-ctest.log), [full output](cpp-correctness-2026-11-07/asan-tests.log) |
| clang-tidy high-signal gate | 111 production sources, 4 fail-closed checks, 0 failures | [Gate log](cpp-correctness-2026-11-07/clang-tidy.log) |
| clang-format / diff whitespace | Passed on the 14 changed C++ files | `clang-format --dry-run --Werror`; `git diff --check` |
| Assurance, structure and documentation | Passed | [Validator output](cpp-correctness-2026-11-07/validators.log) |

Commands (from the repository root; local tool binaries live under `.tools/venv/bin`):

```sh
cmake --build build/macos-debug --target glifistore_tests glifistore_allocation_fault_tests -j 6
build/macos-debug/glifistore_tests
build/macos-debug/glifistore_allocation_fault_tests
cmake --build build/macos-asan --target glifistore_tests glifistore_allocation_fault_tests -j 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/macos-asan \
  -R '^(glifistore_tests|glifistore_allocation_fault_tests|glifistore_c_abi_tests|glifistore_c_abi_durable_tests|glifistore_installed_cpp_interop)$' \
  --output-on-failure -j 2
python engineering/tools/run_clang_tidy_gate.py --build-dir build/macos-debug \
  --clang-tidy "$PWD/.tools/venv/bin/clang-tidy" --jobs 4
python engineering/tools/validate_assurance.py --write-generated
python engineering/tools/validate_structure_debt.py
python engineering/tools/validate_documentation.py
```

The ASan selection includes the allocation-fault suite, C11 ABI tests, durable ABI tests and an
installed C++ consumer interoperability run. None of these local results is hosted CI evidence.

## Residual scope

The campaign runs on macOS ARM64 with Apple clang 21. Linux/BSD and TSan are separate campaigns.
LeakSanitizer is unsupported on this host; ASan runs with `detect_leaks=0`. Allocation fault tests
model selected one-shot failures and response-allocation descheduling; arbitrary persistent global
OOM, every TLS/backend failure schedule and cross-SDK allocation semantics are not certified.
