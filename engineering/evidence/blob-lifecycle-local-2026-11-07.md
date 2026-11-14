# Local blob lifecycle corrections — 2026-11-07

Evidence class: **local**, macOS ARM64. GlifiStore remains an architectural prototype.
No CI, release acceptance, complete resource bound or E3/E4 certification is claimed.

## Scope

Corrections to the embedded upload lifecycle under [ADR 0042](../../docs/adr/0042-large-blob-storage.md)
and [the blob specification](../../docs/spec/large-blob-storage-v1.md):

- Track exclusive staging ownership so abort, failed write and close remove their temporary files.
- Preserve terminal failure and return transfer credits after cleanup attempts, including exceptions.
- Release key and payload allocations before terminal credits can be reused by another handle.
- Reserve the peak staging/final extent before file creation, with checked arithmetic.
- Preserve configured syscall fault hooks when syncing the blob parent directory.
- Classify blob filesystem checkpoints in the allocation-fault observer and use version 3 as
  the unknown record version after the accepted version 2 extension.

The descriptor admission, payload sync/install ordering, acknowledgement points and legacy v1
encoding are unchanged. Failed cleanup keeps orphan byte/file accounting for conservative GC.

## Regression evidence

The four initial lifecycle regressions failed on the pre-fix implementation:
[retained failing output](blob-lifecycle-2026-11-07/regression-red.log).
Three additional tests cover exact peak quota with reopen, closing an active staged upload
and commit concurrent with a chunk exception. The transfer mutex remains held through
exception terminalization so a waiting commit cannot observe the partial upload.
The complete command results and source/log hashes are recorded in
[the campaign summary](blob-lifecycle-2026-11-07/summary.json).

## Local verification

| Check | Result | Evidence |
| --- | --- | --- |
| Debug general suite | 752 passed, zero failures | [Log](blob-lifecycle-2026-11-07/debug-main-tests.log) |
| ASan + UBSan general suite | 752 passed, zero failures | [Log](blob-lifecycle-2026-11-07/asan-main-tests.log) |
| Blob integration suite | 18 passed in Debug and ASan + UBSan | [Debug](blob-lifecycle-2026-11-07/debug-blob-tests.log), [sanitizers](blob-lifecycle-2026-11-07/asan-blob-tests.log) |
| Final lifecycle regressions | Seven passed after final native corrections | [ASan + UBSan](blob-lifecycle-2026-11-07/asan-lifecycle-final.log) |
| Final shared-workspace changes | Nine targeted ASan + UBSan tests passed after rebuilding the later identity/memory-budget and fixture changes | [Build](blob-lifecycle-2026-11-07/final-targeted-build.log), [tests](blob-lifecycle-2026-11-07/final-targeted-tests.log), [static checks](blob-lifecycle-2026-11-07/clang-tidy-latest.log) |
| Later independent golden fixture | Passed after fixing its incomplete aggregate initializer | [ASan + UBSan](blob-lifecycle-2026-11-07/asan-golden-test.log), [build](blob-lifecycle-2026-11-07/asan-golden-build.log) |
| Crash, C ABI, ABI symbols, allocation-fault campaign | Five CTest targets passed in both builds | [Debug](blob-lifecycle-2026-11-07/debug-quality.log), [sanitizers](blob-lifecycle-2026-11-07/asan-quality.log) |
| Production static analysis | 116 sources, eight checks; final changed sources also rechecked | [Gate](blob-lifecycle-2026-11-07/clang-tidy.log), [final sources](blob-lifecycle-2026-11-07/clang-tidy-final-sources.log) |
| Formatting | Pinned clang-format 21.1.8, 417 files | [Write](blob-lifecycle-2026-11-07/format-write.log), [check](blob-lifecycle-2026-11-07/format-check.log) |

Both builds enabled warnings as errors. The full suites ran before the last defensive identity
guard and brace/name alignment; final lifecycle tests and static analysis cover those changes.
The later independent golden fixture was run separately, rather than being counted in the
18-test blob snapshot. ASan used `detect_leaks=0` because this host does not support LSan;
UBSan used `halt_on_error=1`. TSan was not run in this campaign.

## Residual scope

Local tests do not establish sustained allocation or unlink failure behavior, complete memory
accounting, queue saturation, concurrent close/GC proof, cross-platform filesystem guarantees,
power-loss safety or inline performance budgets. Other blob and compaction development continues in the shared workspace. The machine-readable
summary distinguishes the validation checkpoint from later source changes; passing logs do not
certify untested subsequent saves or the complete feature. The blob gates stay open.
