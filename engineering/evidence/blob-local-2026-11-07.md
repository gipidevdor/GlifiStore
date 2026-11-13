# Embedded Large Blob Storage — local campaign, 2026-11-07

Authority: [ADR 0042](../../docs/adr/0042-large-blob-storage.md),
[Blob v1 specification](../../docs/spec/large-blob-storage-v1.md),
[requirements](../requirements/large-blobs.yaml) and
[open gates](../gates/large-blobs.yaml).
Evidence class: local macOS arm64, architectural prototype, E2 process termination only.
This campaign provides no hosted CI, release acceptance or E3/E4 certification.

## Implemented scope

New durable paired embedded Stores can select `StoreConfig::enable_blobs = true`.
Single-item PUT/GET/ERASE preserve logical values up to 67,108,864 bytes, excluding
the key and metadata. Streaming handles bound chunk I/O and preserve a selected version.
The default v1 profile remains available; the blob profile explicitly uses persistence
family v2 with Blob File v1 and BlobRef v1. Existing Stores are not silently upgraded.

The implementation includes explicit codecs, chunk CRC32C and SHA-256, verified
payload installation before descriptor admission, monotonic identities, historical-root
recovery, conservative collection, descriptor compaction and fenced backup verification.
The final correction makes read-only verification refuse a missing blob namespace
without creating it. Only ordinary Store initialization can create the initial directory.
Daemon, wire blob messages, SDK and C ABI enablement remain deferred.

## Results

| Check | Result | Retained evidence |
| --- | --- | --- |
| General native suite | 752 tests, zero failures at the earlier integration checkpoint | [Native log](blob-2026-11-07/native-debug.log) |
| Final ASan + UBSan blob suite, TLS disabled | 22 tests, zero failures or sanitizer reports | [CTest](blob-2026-11-07/blob-asan-final.log), [full output](blob-2026-11-07/blob-asan-detail.log) |
| Final SIGKILL crash matrix under ASan + UBSan | 51 cases passed: 17 boundaries in each of sync, strict group and periodic modes | [Full output](blob-2026-11-07/blob-asan-detail.log) |
| Final native read-only verifier regression | One test, zero failures | [Native regression](blob-2026-11-07/readonly-native.log) |
| Native C ABI, ABI and existing crash selection | 17 targets passed; allocation-fault target exceeded its 180-second limit | [Selection](blob-2026-11-07/legacy-ctest.log) |
| Allocation-fault CTest retry | Repeated the 180-second timeout; this is retained as a failed check | [Retry](blob-2026-11-07/allocation-recheck.log) |
| Allocation-fault direct diagnostic | Passed, exit 0, in 183.89 seconds; does not erase either CTest timeout | [Complete run](blob-2026-11-07/allocation-direct.log) |

The blob suite covers the accepted size boundaries, maximum key with a full 64 MiB
value, inline/blob replacement, independent fixtures, expired-version read pins,
group/periodic commits, historical corruption, backup, compaction without Segment-count
gain, quotas, staging cleanup, terminal exceptions and concurrent commit failure.
The crash matrix checks old-or-complete recovery and requires the complete value after
strict ACK or explicit flush. Payload preparation alone cannot change the recovered key.

The initial SIGKILL harness raced process-directed SIGKILL with `_exit(125)` on the
calling I/O thread. The [superseded failure](blob-2026-11-07/blob-ctest-final.log) is
retained. The corrected harness waits for SIGKILL, and the complete final matrix passes.
That initial harness result is not counted as a successful crash campaign.

## Reproduction and source identity

Build: C++23, AppleClang 21, CMake 4.4.5, Debug, warnings as errors, fault injection,
ASan + UBSan with TLS explicitly disabled. See
[environment](blob-2026-11-07/environment.txt),
[sanitizer configuration](blob-2026-11-07/sanitizer-config.txt),
[source hashes](blob-2026-11-07/source-hashes.json) and
[validation patch](blob-2026-11-07/validation.patch).
The [machine-readable summary](blob-2026-11-07/summary.json) records the base revision,
binary hashes and campaign outcomes; [artifact checksums](blob-2026-11-07/artifacts.sha256)
cover the retained bundle.
The source tree already contained concurrent changes. The general suite ran before the
last blob-only verifier correction; the final blob run includes that correction.
Only whitespace formatting followed the sanitizer build.

```sh
cmake --build build/blob-asan --target glifistore_blob_tests glifistore_crash_blob -j4
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/blob-asan \
  -R '^(glifistore_blob_tests|glifistore_crash_blob)$' --output-on-failure
python3 scripts/generate_blob_fixtures.py --check
python3 engineering/tools/validate_assurance.py
```

## Residual limits

The allocation timeout prevents claiming the complete selected regression gate passed.
The [sample taken during diagnosis](blob-2026-11-07/allocation-sync-sample.txt) showed
kernel file synchronization in the existing v1 allocation-compaction setup. A direct
diagnostic run then passed in 183.89 seconds. These observations do not establish why
the CTest budget was exceeded or waive the timeout; that budget was left unchanged.

Native Linux/FreeBSD/OpenBSD qualification, exhaustive I/O and scheduler faults, sustained
allocation/unlink failures, TSan and long Reader/GC/backup overlap, a retained old-binary
refusal campaign, complete memory census and telemetry, controlled inline A/B latency,
and physical power-loss campaigns remain open. No performance or production-readiness
claim follows from these local functional checks. The embedded blob gate is implemented
but remains open for qualification; the wire gate remains unstarted.
