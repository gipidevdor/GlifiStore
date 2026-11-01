# Network-security bounds hosted CI evidence — 2026-11-01

Source/test commit: `bc0b41edd756b85077e8051ef771db68008fbecc`.
Branch: `codex/fix-ci-and-cleanup`. Contract: [ADR 0041](../../docs/adr/0041-bounded-network-security-lifecycles.md),
GS-SEC-BOUNDS-001, GS-SEC-PROFILE-001 and HAZ-024.

All three workflows completed successfully on this exact source/test commit. The follow-up
assurance commit changes evidence, requirements/gate records and generated documentation only.
GATE-AUTH-TRANSPORT is PROVATA_IN_CI for these retained platform rows.

## Execution matrix

| Workflow | Native environment / scope | Result |
| --- | --- | --- |
| [CI](https://github.com/gipidevdor/GlifiStore/actions/runs/41337377012) | Linux x86_64/ARM GCC/Clang, macOS, strict/release/LTO, SDK and installed-consumer controls | Passed: 11/11 jobs; 74/74 CTest targets in each native build/test row |
| [FreeBSD](https://github.com/gipidevdor/GlifiStore/actions/runs/41337379571) | FreeBSD 14.3, native build/test and installed consumers | Passed: 60/60 CTest targets plus 3/3 installed-consumer tests |
| [OpenBSD LibreSSL](https://github.com/gipidevdor/GlifiStore/actions/runs/41337382031) | OpenBSD 7.9, two deterministic test shards, system LibreSSL and official-client TLS smoke | Passed: 363 + 356 native tests; 14/14 CTest targets per shard; Go TLS PUT/GET smoke |

The platform scripts explicitly exercise the four audit subprocesses: normal JSON/escaping/quiet/
storm, full blocking sink with single-Reader HEALTH and actual Server destruction, closed sink
with default SIGPIPE, and nonblocking backpressure with expanded fields. The general suite covers
TLS handshake/slot/deadline, output progress, denial quotas, handoff and ordinary client outcomes.

## Campaign history and local controls

[Local campaign](security-bounds-local-2026-11-01.md): 740 native tests plus the four audit
subprocesses passed after the final sink error-handling change. TLS-off and ASan/UBSan audit
subprocesses passed. The BSD-portable denial regression also passed locally with ASan/UBSan.
Production sources under `src/` and `include/` are identical between `67ad42f` and `bc0b41e`.
`2eb28d5` changes only the test helper to use a scoped `std::thread` and atomic stop flag.
`bc0b41e` makes the positive drain control portable across TCP scheduling/ACK timing: a
1,000 ms budget replaces 80 ms, and EOF plus the request-timeout counter are now required.
The separate stopped-output regressions and their expiry/counter/slot assertions are unchanged.

The first FreeBSD run on `67ad42f` failed to compile the test's `std::jthread`/`std::stop_token`
with its libc++. [Failed run](https://github.com/gipidevdor/GlifiStore/actions/runs/41335818185).
That failure was retained, corrected in `2eb28d5`, and all workflows were dispatched again on
the same updated source/test commit. The subsequent
[FreeBSD run](https://github.com/gipidevdor/GlifiStore/actions/runs/41336514547) passed the four
audit subprocesses and other new security regressions, but failed that 80 ms positive drain
control. `bc0b41e` corrected the control; it passed locally before the final dispatch.
Superseded and partially successful runs are not counted as complete hosted proof.

## Retained evidence

Raw job logs and downloaded native logs are retained in the target-bound managed remediation
collection under `artifacts/fixes/audit-isolation/`. GitHub native artifacts have a 14-day
retention window; the copies, checksums and selected output retained here preserve the evidence
identity beyond that window. ZIP digests below come from GitHub's artifact API; plaintext log
digests come from the retained downloaded files.

| Native artifact | GitHub ZIP SHA-256 | Retained plaintext log SHA-256 |
| --- | --- | --- |
| [FreeBSD](https://github.com/gipidevdor/GlifiStore/actions/runs/41337379571/artifacts/13095768904) | `6a8bb52ac6b0ea6ad3ab9be4a97f520c9a561cb40043c6d55aa1d39263b60917` | `1adab69558f57386d1924fe33ab25afcd0445c3303ef8808342d2adfed14542c` |
| [OpenBSD 1-of-2](https://github.com/gipidevdor/GlifiStore/actions/runs/41337382031/artifacts/13095933823) | `5af9d4d6a2e0695bfb2022c227ad347c3a14db44db332c4657900f029e7f1dc5` | `f0bfea747c86de0b66f6244b0a682d5cbf335388272ea095cb6820f299220f8f` |
| [OpenBSD 2-of-2](https://github.com/gipidevdor/GlifiStore/actions/runs/41337382031/artifacts/13096331206) | `1b90bda94fca66ee092bf2cd72438392917cef163457b3c55512eaf63f89fab1` | `961a417f9496cf06ef8973e46f07efb413b962721bbb4c41a31eaffab536c9ed` |

Selected OpenBSD native output below is normalized only by removing timestamps/CTest prefixes
and duplicate audit lines from the second execution. The complete original logs are retained:

```text
[PASS] security bounds closing denied stream yields despite continued input
[PASS] security bounds capability prefix and stats denials share principal quota
[PASS] security bounds denied requests and denied probes consume attempt quota
[PASS] security bounds stalled output expires and releases connection slot
audit sink normal passed
audit sink blocked passed
audit sink closed passed
audit sink nonblocking passed
```

Linux optimized and macOS each passed all 74 CTest targets. Their retained job-log SHA-256 values
are respectively `0951073ebab001a240f71f9e20ed46d02ad79be62e5293932fa603351a418ca3` and
`be8a8c09dbc5abd3dba19737eb2b51a3d84fc66092477f552148a1c973dfd6e9`.

The SDK job passed both source-tree and installed-artifact secure-profile interop. Its retained
full log has SHA-256 `96ec1b0cad5f7e628c02b0f68c7be723fb53b53d7c3d91d50e9e0d9778868426`.
The workflow's SDK bundle upload is push-only, so workflow_dispatch retained that proof in the
job log rather than an uploaded SDK evidence bundle. ISO C++23/emitted-flags/ELF hardening
verification and all 74 strict CTest targets passed; that job log has SHA-256
`12dd1af851ff81e5d77db1ea12ceda6b2d25d82418c5f96589caaece9ae4e432`.
LTO also passed all 74 CTest targets; its retained job-log SHA-256 is
`7975cc6a365f934ed16ffe5c2efd7509417eb134693fee9237323cc479dffbf5`.

## Claim boundary

These are software/CI proofs for the named commit and platform rows. They do not establish
production readiness, hostile-public deployment, adversarial multi-tenant certification,
durable audit delivery or physical E3/E4 durability. Audit remains best effort: a blocked sink
retains one process-lifetime emitter and a fixed queue; later events may be suppressed and
pending events lost at exit. Hard I/O errors disable the sink. Packet-level shaping and the
explicit trusted configuration with both output-timeout knobs zero remain outside the bound.
