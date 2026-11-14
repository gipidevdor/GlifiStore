# Local correctness-assurance campaign — 2026-08-30

Evidence class: **local**. This record covers source commit
`9ba73973dc67b92dd9b33ca7f0eb726469c161b9`; it is retained by a later evidence-only commit.
It does not promote a gate to `PROVATA_IN_CI`, certify a platform durability row, accept ADR 0036,
or raise the architectural-prototype claim ceiling.

## Environment and scope

- Host: Apple M4, arm64, macOS 26.6.2 (25G83), APFS, AC power.
- Toolchain: Apple clang 21.0.0, clang-tidy 21.1.6, clang-format 21.1.8, CMake 4.4.0,
  Python 3.13.15.
- Production analysis scope: 109 compile-database sources and 249 supported broad-profile checks.
- Runtime scope: Debug, warnings-as-errors, diagnostic libc++ hardening, ASan+UBSan, and TSan.
- Proof scope: stateful Store model, bounded linearizability histories, systematic filesystem failure
  positions, shutdown/reclamation stress, token-generation wrap, bounded queues, and explicit atomic
  memory-order inventory.

The reproducible entry point is `./scripts/dev.sh correctness`. It prints every executed command,
distinguishes required failures from optional unavailable capabilities, and stores its local summary
under `build/correctness/`. The first end-to-end invocation was interrupted after ASan while the
driver itself was being corrected; every constituent stage was subsequently executed against the
final source, and the driver and its Python tests passed. This record therefore reports the stage
results rather than presenting that interrupted invocation as one uninterrupted run.

## Defects confirmed and repaired

| Area | Confirmed defect | Repair and regression proof |
| --- | --- | --- |
| Linearizability oracle | Histories could be accepted by mapping unexpected failures to an unconstrained `other_error` transition. | The model now represents exact success/missing/rejected outcomes, includes the pre-store rejected no-op, and minimizes only against the corrected oracle. The ordinary suite and 1,280 additional histories passed. |
| Connection lifecycle | A slot generation wrapping from `UINT32_MAX` to zero could eventually revalidate a stale token. | The slot is permanently retired at the terminal generation; a boundary regression test verifies stale-token rejection. |
| Worker routing | Independently published routing payload and revision could be observed as a torn pair under concurrent writers. | Writers are serialized and the revision/payload publication is sequentially consistent and non-wrapping. A four-reader, 100,000-update regression passed. |
| MPSC queue | Signed sequence differences could overflow near counter wrap. | Comparisons use unsigned modular ordering and the queue enforces the capacity precondition; boundary tests passed. SPSC capacity receives the matching bound. |
| `Result<void>` | Calling `error()` on success dereferenced an empty optional. | The accessor now fails deterministically with `std::bad_optional_access`; both lvalue and rvalue regression cases passed. |

Preventive hardening also covers mutation-slot counter exhaustion at quiescence, no-allocation
shutdown/disk-read failure paths, checked or saturating size/capacity accounting, and ownership-safe
optional extraction. None of these repairs changes persistence format v1, wire protocol v2,
acknowledgement points, Manifest authority, public mutation visibility, or recovery ordering.

## Static-analysis closure

The broad production sweep started with 977 findings on its first profile (`A=7`, `B=75`,
`C=895`). After removing checks that were unsupported or structurally unsuitable for this
compile-database pass, the comparable 249-check profile started at `A=4`, `B=73`, `C=184`.
The final sweep completed all 109 sources with no tool failures and classified:

| Class | Meaning | Final count |
| --- | --- | ---: |
| A | confirmed correctness or undefined-behaviour defect | 0 |
| B | actionable hardening or maintainability defect | 0 |
| C | intentionally accepted style/ABI/layout diagnostic | 184 |
| D | exact, reviewed false positive with a versioned rationale | 48 |

The 184 class-C findings are 128 `#pragma once` portability preferences, 47 easily-swappable
parameter reports, 5 padding observations, and 4 branch-clone reports. The 48 class-D occurrences
are matched by exact check/file/message/line rules in the versioned triage contract; broad wildcard
suppressions are not accepted. The complete machine-readable result is retained in
[`correctness-clang-tidy-2026-08-30.json`](correctness-clang-tidy-2026-08-30.json).

The independent high-signal gate also passed all 109 production sources with its four fail-closed
checks. The explicit memory-order audit covers 1,190 atomic operations across 15 documented
concurrency domains and rejects an unclassified or implicit operation.

## Dynamic and adversarial matrix

| Matrix | Result | Detail |
| --- | --- | --- |
| Repository verification | PASS | assurance: 33 requirements, 33 hazards, 28 gates, 3 waivers; documentation: 259 Markdown files / 1,041 local links; 114 Python/tooling tests; structure, compatibility, pinned actions, claims, durability, and memory-order checks passed |
| Debug CTest | PASS | 54/54, 412.22 s; final main suite 155.40 s, allocation-fault row 88.94 s, crash/recovery row 141.40 s |
| Strict warnings-as-errors | PASS | 54/54, 330.34 s; final rebuilt main suite 156.41 s |
| Diagnostic libc++ hardening | PASS | 54/54, 396.14 s |
| ASan+UBSan | PASS | 54/54, 525.78 s; final rebuilt main suite 159.04 s; no sanitizer report |
| TSan | PASS | 53/53, 1,218.46 s; main suite 251.08 s, crash/recovery 881.61 s; final rebuilt main suite 258.90 s; no race report |
| High-signal clang-tidy | PASS | 109/109 production sources |
| Broad clang-tidy | PASS | 109 sources, 249 checks, `A=0`, `B=0`, `C=184`, `D=48`, no tool failures |
| Linearizability repetition | PASS | 20 runs × 64 conclusive histories = 1,280 histories, in addition to the ordinary suite |
| Fault-position enumeration | PASS | every reached online-compaction filesystem failure position, with invariant and recovery validation |
| Shutdown/reclamation torture | PASS | 24 deterministic seeds, plus sanitizer coverage |
| Stateful model/property test | PASS | randomized PUT/GET/ERASE/reopen sequences against the reference model |

The sanitizer driver capability-probes non-default options before use. This runtime supports and ran
strict string checks, stack-use-after-return, initialization-order, and allocation/deallocation
mismatch checks. LeakSanitizer is unavailable on this Apple runtime. The pointer compare/subtract
instrumentation compiles, but its functional probe aborts inside a valid libc++ `std::vector`
one-past-end operation; enabling it would make the matrix fail on a runtime/library false positive,
so it is explicitly reported as unavailable rather than silently omitted.

## Availability gaps and residual risk

- This host has no independent GCC/libstdc++ toolchain, scan-build, cppcheck, or Valgrind. Compiler
  and platform diversity remain CI responsibilities.
- The local Apple clang installation has no linkable libFuzzer runtime. The new stateful Store
  fuzzer and its seed corpus are assigned to the Linux sanitizer CI matrix, not this local record.
- The adversarial histories are deterministic, bounded tests; they are not a formal proof over the
  C++ memory model, exhaustive counter-wrap exploration, or a multi-hour fairness campaign.
- Crash/recovery tests exercise process termination and ordered filesystem fault points. They do not
  demonstrate physical power-loss durability or close any E3/E4 platform row.
- The broad analyzer's reviewed C/D findings remain debt inventory, not a claim that every possible
  static-analysis warning is absent.

The net result is a materially stronger architectural prototype with five confirmed correctness
defects removed and broader executable proof. The remaining limitations are explicit and keep the
project below production-readiness and durability-certification claims.
