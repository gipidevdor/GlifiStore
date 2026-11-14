# ADR 0036 fixed-shell local evidence — 2026-08-27

Status: local candidate campaign; not CI proof, release evidence, or a production-runtime claim.

## Scope

The test-only slot pool was composed with a fixed bank that backs the real incremental
`PairReadGeneration` co-allocation. The official runtime still uses Alternative A. No ACK point,
recovery rule, persistence v1 byte, wire v2 outcome, or routing behavior changed.

The focused cases prove:

- one occupied storage never falls back to heap allocation;
- `PairReadGeneration`, embedded `DeltaState`, and control block reuse the same fixed address;
- storage remains occupied through final weak-owner destruction;
- allocator ownership keeps backing bytes alive after the external owner is released;
- a reservation selects the storage with the same slot index;
- Reader adoption and reclaim release that exact storage for slot reincarnation;
- the real generation remains readable after publication.

This does not assert that the complete PUT path is allocation-free. Delta pages, COW spines, arena
blocks, external keys, base rebuilds and pins remain independently allocated when required.

## Local results

Host: Apple arm64 macOS, Apple Clang, existing repository presets.

| Configuration | Filter | Result |
| --- | --- | --- |
| Release | `ADR 0036` | 36/36 pass |
| ASan + UBSan | `ADR 0036 direct slot pool` | 5/5 pass |
| TSan | `ADR 0036 direct slot pool` | 5/5 pass |
| Release | `paired` | 51/51 pass |

The storage uses a checked 512-byte, 64-byte-aligned candidate block. Cross-toolchain size and
alignment evidence, official runtime integration, crash/fault matrices, full multi-OS CI, and the
worker-affine V11/V12 A/B remain open.

The local construction-only diagnostic is retained under
`benchmarks/results/adr0036-fixed-shell-2026-08-27/`. Its fixed-shell median is 2.91% below the
default `make_shared` path. That is below the ADR's hard 5% rejection threshold but is not V11/V12
evidence and does not justify runtime integration.

The inline-owner extension removes backing-storage shared ownership and atomics under a structural
Writer-only lifetime. Its focused concurrent stress passes in Release, ASan+UBSan and TSan. In the
protocol-equivalent microbenchmark it improves the shared-backing pool by 7.53%, but remains 7.09%
behind plain `make_shared` in the same diagnostic campaign. This branch is therefore evidence for
lifetime/cost attribution, not a production candidate ready for V11/V12.

The direct-object extension shares production validation/delta construction but removes the
generation control block. Differential and rejected-build tests pass in all three configurations.
Its local construction median is +17.08% over `make_shared`; the result does not include concurrent
Reader reclamation and therefore remains directional evidence only.

The direct slot-pool extension adds the missing bounded QSBR composition: direct epoch-zero
construction, reservation, release/acquire token, cold-borrow safe frontier, Writer reclaim and
mandatory terminal shutdown. The 10,000-publication Reader/Writer stress is clean under focused
Release, ASan+UBSan and TSan runs. Its synchronous protocol median is 3,186,299 publications/s,
7.43% above `make_shared` and 8.28% below the direct ring. It still lacks the two-thread
worker-affine V11/V12 matrix, merge ownership, durable refresh and runtime integration.

The follow-up two-thread runner compares identical 65-slot protocols with shared versus direct
generation ownership. The final 11-repeat local median is 1,275,537 versus 1,190,497
publications/s (+7.14%); aggregate ns/publication improves 6.67%, sampled p50 is unchanged and
sampled p99 regresses 5.86%. Affinity was requested but unavailable on the macOS host. Both
alternatives complete a 5,000-publication runner under ASan+UBSan and TSan. This is local candidate
evidence only and does not close V11/V12.

The additional Reader-work row performs a real immutable-generation GET on every adoption. Direct
construction reaches 919,357 versus 817,965 publications/s (+12.40%), improves sampled publication
p50/p99 by 9.09%/9.54%, and keeps sampled GET p50/p99 unchanged at 83/250 ns. The value is two
bytes and L1-hot; protocol/socket work, wider datasets and verified affinity remain open.
