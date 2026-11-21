# Read-capacity prototype: local evidence, 2026-11-21

Architectural prototype only. ADR 0049 accepts an opt-in implementation; no capacity profile,
threshold, native platform row or release gate is qualified by this record. Default helper cap
is zero. Fixed-helper experiments are explicit and are not adaptive-policy measurements.

The pure controller has tests for unknown/stale/changed profiles, contiguous windows, hard
credits through arming/draining, counterfactual quiet retirement, equal-weight rotation and
identical replay of ordered traces. The volatile TCP tests cover GET/PUT/GET/ERASE ordering,
misses, empty and large values, and unknown-capacity fallback. An adverse-scheduling seam stops
a helper after selecting its view, publishes newer versions, exercises primary fallback, then
completes or cancels the old read. Both ordinary and slot-pool generation modes are covered.

The offered-rate driver uses the canonical SDK wire codec. Scheduled times are independent of
responses; admission and transport memory are bounded, missed offers are counted, and every
returned request identity/status/owner/value is checked. Latency starts at scheduled arrival.
A deliberately slow-server regression proves the sender does not become a closed-loop capacity
measurement. Reports always retain `qualification=unqualified-local-experiment`.

Machine: local macOS arm64, 10 logical CPUs. Loopback client CPU competes with the server.
Measurements below are bounded pilot experiments, not sustainable-capacity curves or sibling
isolation proofs. Server CPU includes startup/drain; no energy or financial return is inferred.

Validation logs and raw experiment inputs/results are retained beside this record. Failed
attempts and GDev audit records are retained locally under `.gdev/assurance/external-evidence/`.
No local procedure constitutes human review, certification or release acceptance.

## Results on the final runtime sources

Each row offers the stated rate for three seconds, with eight loopback clients. All rows
complete every scheduled request correctly; these are low-load pilots, not saturation curves.

| Value bytes | Helpers | Offered GET/s | Completed | Missed | p99 scheduled-to-response (µs) | Server CPU (s) | Delegated |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 0 | 15000 | 45000 | 0 | 1240.8 | 0.579 | 0 |
| 1024 | 1 | 15000 | 45000 | 0 | 1173.4 | 0.793 | 15363 |
| 1024 | 2 | 15000 | 45000 | 0 | 1160.6 | 0.899 | 26191 |
| 1024 | 4 | 15000 | 45000 | 0 | 1151.7 | 1.106 | 41632 |
| 262144 | 0 | 500 | 1500 | 0 | 3295.8 | 0.273 | 0 |
| 262144 | 1 | 500 | 1500 | 0 | 3338.6 | 0.315 | 1500 |
| 262144 | 2 | 500 | 1500 | 0 | 3332.6 | 0.313 | 1500 |
| 262144 | 4 | 500 | 1500 | 0 | 3377.8 | 0.292 | 1500 |

At 1KiB / 15000 GET/s, fixed helpers add CPU cost while the primary already serves demand.
This does not justify automatic activation or a qualified capacity profile. Larger-value
pilots likewise do not establish a throughput gain. No energy or financial estimate is made.

Validation: the complete Release suite passed 795 tests before the final controller/telemetry
refinements. Current-source targeted tests pass 10 cases in Release and 11 cases with both
ASan/UBSan and TSan, including ordinary/slot-pool generations and stale-token slot reuse. The
offered-rate driver has two regression tests. Debug clang-tidy passed all 119 production sources;
the two final changed production sources also pass the high-signal checks in Release. A complete
Release tidy run reports existing diagnostic-only dead stores in unchanged
`src/persistence/runtime_catalog_read_ops.cpp` when timing is compiled out. That failed attempt is
retained locally; it was not suppressed, reclassified as a pass or fixed by this change.

Earlier pilot measurements, an initially overstrict delegated-count assertion, the driver
end-window counting refinement, and an unsupported macOS ASan leak-detection attempt remain
in the local audit archive. Final sanitizer checks use the supported platform configuration.
The ordinary response still falls back when earlier ACK output is pending, so not every GET
in a mutation pipeline is expected to be delegated. No client error was added for helper pressure.
