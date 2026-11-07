# Retained evidence drops for tagged claims (Phase D+).
# Populate from CI artifacts / campaign records; do not invent E3/E4.

- [`blob-lifecycle-local-2026-11-07.md`](blob-lifecycle-local-2026-11-07.md) records local staging
  cleanup, terminal exception, peak quota and sync-hook regressions for embedded blobs.
  It covers a limited lifecycle subset and does not close the blob or durability gates.
- [`compaction-delta-local-2026-11-07.md`](compaction-delta-local-2026-11-07.md) records the isolated
  v1 bounded-delta native/sanitizer/allocation/crash campaign and controlled staging smoke samples.
  It is local macOS prototype evidence, without a p99.9 improvement or release acceptance claim.
- [`code-style-local-2026-11-07.md`](code-style-local-2026-11-07.md) records the shared pinned
  formatter, native source coverage and eight-check static-analysis campaign, including the
  concurrent-feature rebuild limitation. It is local evidence only.
- [`cpp-correctness-local-2026-11-07.md`](cpp-correctness-local-2026-11-07.md) records the
  local C++ allocation, deadline, observer, aliasing and cold-read lifetime repairs. It does not
  promote a gate or provide CI, release or durability certification.
- [`dependabot-local-2026-11-07.md`](dependabot-local-2026-11-07.md) records local CodeQL
  revision/grouping regressions and TLC checks for Dependabot workflow repairs. It is neither
  hosted CI nor release evidence.
- [`sdk-installed-secure-profile.md`](sdk-installed-secure-profile.md) defines the temporary CI
  evidence bundle for the installed six-SDK secure-profile matrix. It is not a tagged claim drop.
- [`adr0036-slot-token-local-2026-08-27.md`](adr0036-slot-token-local-2026-08-27.md) records a local,
  prototype-only Release/sanitizer campaign. It is neither CI nor release evidence.
- [`paired-generation-admission-local-2026-08-27.md`](paired-generation-admission-local-2026-08-27.md)
  records the local Alternative A retire-bound campaign. It is neither CI nor release evidence.
- [`adr0036-fixed-shell-local-2026-08-27.md`](adr0036-fixed-shell-local-2026-08-27.md) records the
  local fixed-shell, direct-object and two-thread publication candidate campaigns. It is neither CI
  nor release evidence.
- [`adr0036-production-slot-local-2026-08-27.md`](adr0036-production-slot-local-2026-08-27.md)
  records local Wave 1 production-flag V1/V5/V6/V7/V9/V10 coverage. ADR 0036 remains proposed;
  this is neither CI nor release evidence.
- [`quality-hardening-local-2026-08-30.md`](quality-hardening-local-2026-08-30.md) records the local
  documentation, formatting, static-analysis, strict, ASan+UBSan, and TSan hardening campaign. It
  is neither CI nor release evidence.
- [`correctness-assurance-local-2026-08-30.md`](correctness-assurance-local-2026-08-30.md) records
  the local adversarial correctness campaign, confirmed defect repairs, analyzer triage, and
  capability gaps. Its machine-readable broad clang-tidy result is retained in
  [`correctness-clang-tidy-2026-08-30.json`](correctness-clang-tidy-2026-08-30.json). Neither file
  is CI or release evidence.

- [`platform-durability/`](platform-durability/) reserves per-row evidence path placeholders
  (APFS/ext4/XFS/UFS/ZFS/FFS). Rows are **not** E3/E4 certified; see each subdirectory README.

Tagged release promotion uses the machine-readable contract in
[`docs/distribution/release-evidence.md`](../../docs/distribution/release-evidence.md). Evidence JSON
and every referenced log are carried as sealed release candidate files; this directory stores only
durable pointers or reviewed campaign records, never hand-written replacements for missing CI runs.

Release retention checklist (Wave 5 scaffolding, no tagged drops yet):
[`release/README.md`](release/README.md).
