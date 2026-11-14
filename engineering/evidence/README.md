# Retained evidence

Durable pointers and reviewed campaign records behind GlifiStore's claims. They are populated from
CI artifacts and campaign records, never written by hand in place of a missing CI run, and no E3 or
E4 result is ever invented.

Tagged release promotion follows the machine-readable contract in
[release evidence](../../docs/distribution/release-evidence.md): evidence JSON and every log it
references are carried as sealed release-candidate files. The release retention checklist is in
[`release/`](release/README.md); no tagged drop exists yet.

## Hosted CI

- [`security-bounds-ci-2026-11-01.md`](security-bounds-ci-2026-11-01.md): three network-security
  bounds workflows that completed successfully on one exact source and test commit
  ([ADR 0041](../../docs/adr/0041-bounded-network-security-lifecycles.md), `GS-SEC-BOUNDS-001`,
  `GS-SEC-PROFILE-001`, `HAZ-024`). `GATE-AUTH-TRANSPORT` is `PROVATA_IN_CI` for its retained
  platform rows.
- [`sdk-installed-secure-profile.md`](sdk-installed-secure-profile.md) defines the temporary CI
  evidence bundle for the installed six-SDK secure-profile matrix. It is not a tagged claim drop.

## Platform durability

[`platform-durability/`](platform-durability/) reserves an evidence path per row: APFS, ext4, XFS,
UFS, ZFS and FFS. No row is E3 or E4 certified; each subdirectory README says what it holds.

## Inventories

[`memory-order-inventory-2026-08-30.json`](memory-order-inventory-2026-08-30.json) is the
machine-readable record behind the
[memory-order inventory](../../docs/assurance/memory-order-inventory.md).

## Local campaigns

Local records are prototype evidence
([evidence taxonomy](../../docs/assurance/evidence-taxonomy.md)): none is hosted CI or release
evidence. Each 2026-11-07 record keeps its raw logs in a directory with the same subject and date,
such as `blob-2026-11-07/`.

- [`blob-local-2026-11-07.md`](blob-local-2026-11-07.md): the embedded 64 MiB integration,
  sanitizer and 51-case SIGKILL campaign, including the allocation-test timeout. It does not close
  the blob gates.
- [`blob-lifecycle-local-2026-11-07.md`](blob-lifecycle-local-2026-11-07.md): staging cleanup,
  terminal exception, peak quota and sync-hook regressions for embedded blobs. A limited lifecycle
  subset; it does not close the blob or durability gates.
- [`compaction-delta-local-2026-11-07.md`](compaction-delta-local-2026-11-07.md): the isolated v1
  bounded-delta native, sanitizer, allocation and crash campaign, with controlled staging smoke
  samples, on macOS. No p99.9 improvement or release acceptance claim.
- [`code-style-local-2026-11-07.md`](code-style-local-2026-11-07.md): the shared pinned formatter,
  native source coverage and the eight-check static-analysis campaign, including the
  concurrent-feature rebuild limitation.
- [`cpp-correctness-local-2026-11-07.md`](cpp-correctness-local-2026-11-07.md): C++ allocation,
  deadline, observer, aliasing and cold-read lifetime repairs. It promotes no gate and provides no
  CI, release or durability certification.
- [`dependabot-local-2026-11-07.md`](dependabot-local-2026-11-07.md): CodeQL revision and grouping
  regressions and TLC checks for the Dependabot workflow repairs.
- [`security-bounds-local-2026-11-01.md`](security-bounds-local-2026-11-01.md): the local
  network-security bounds remediation under ADR 0041. It certifies neither production readiness,
  hostile-public exposure nor physical durability.
- [`quality-hardening-local-2026-08-30.md`](quality-hardening-local-2026-08-30.md): the
  documentation, formatting, static-analysis, strict, ASan+UBSan and TSan hardening campaign.
- [`correctness-assurance-local-2026-08-30.md`](correctness-assurance-local-2026-08-30.md): the
  adversarial correctness campaign, confirmed defect repairs, analyzer triage and capability gaps.
  Its broad clang-tidy result is machine-readable in
  [`correctness-clang-tidy-2026-08-30.json`](correctness-clang-tidy-2026-08-30.json).
- [`adr0036-slot-token-local-2026-08-27.md`](adr0036-slot-token-local-2026-08-27.md): a
  prototype-only Release and sanitizer campaign.
- [`adr0036-fixed-shell-local-2026-08-27.md`](adr0036-fixed-shell-local-2026-08-27.md): the
  fixed-shell, direct-object and two-thread publication candidate campaigns.
- [`adr0036-production-slot-local-2026-08-27.md`](adr0036-production-slot-local-2026-08-27.md):
  production-flag V1, V5, V6, V7, V9 and V10 coverage. ADR 0036 remains proposed.
- [`paired-generation-admission-local-2026-08-27.md`](paired-generation-admission-local-2026-08-27.md):
  the Alternative A retire-bound campaign.
- [`wave2-hotpath-local-2026-08-27.md`](wave2-hotpath-local-2026-08-27.md): hot-path residuals:
  lane padding, generation adoption, the Writer wait ladder, zero-heap small GET, combining and
  SPSC backpressure, with their non-goals.
