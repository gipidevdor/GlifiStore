# Bounded compaction delta — local v1 campaign, 2026-11-07

Authority: [ADR 0043](../../docs/adr/0043-bounded-compaction-delta.md),
`GS-PERSIST-DELTA-001`, `HAZ-040`, `GATE-COMPACTION-DELTA`.
Scope: architectural prototype, local macOS arm64; no hosted CI/release acceptance or E3/E4 claim.

## Immutable validation scope

The shared working tree contained concurrent C++ correctness, formatting and persistence-v2 blob
work. Its first broad run reported 751/752 passing, with the v1 golden-fixture unknown-version
assertion failing as the unrelated blob decoder began accepting v2. Fresh sanitizer configuration
also encountered blob sources listed before their implementation was available; copying that
unfinished extension reproduced unrelated blob compilation failures.

To validate this change independently, a disposable source tree was assembled from
`62d87cda086f06b4a11bfaf197cad13bae9ad2ae` plus the bounded compaction implementation/tests and
benchmark instrumentation. Unrelated blob format fields, APIs and working-tree features were
excluded. The [validated patch](compaction-delta-2026-11-07/validated-v1.patch) reconstructs that
scope on the base commit; [source hashes](compaction-delta-2026-11-07/validation-source-hashes.json)
identify the exact modified validation files. This is evidence for the v1 compaction change,
not certification of the independently changing combined workspace.

After the unrelated blob sources became available, the combined workspace was rebuilt and its
suite completed again at 751/752, with the same v1 unknown-version assertion failing. The
[combined-workspace log](compaction-delta-2026-11-07/combined-workspace-tests.log) retains that
integration limitation; this campaign does not alter the blob decoder or its format contract.

## Results

Builds used AppleClang 21, C++23, Ninja, Debug, warnings as errors, TLS enabled and fault injection.
ASan/UBSan and TSan were separate builds. The source snapshot was frozen for all three builds.

| Campaign | Result | Retained output |
| --- | --- | --- |
| Full native suite | 746 tests, zero failures | [Native log](compaction-delta-2026-11-07/native-v1-tests.log) |
| ASan + UBSan, compaction filter | 71 tests, zero failures or sanitizer reports | [Sanitizer log](compaction-delta-2026-11-07/asan-ubsan-compaction.log) |
| TSan concurrency controls | Sync/exclusive/group/periodic reconciliation, paired Store publication, and overflow/retry tests passed without reports | [TSan log](compaction-delta-2026-11-07/tsan-delta.log) |
| Allocation fault executable | Full allocation campaign passed, including forbidden-allocation record/replay with 200 events and binary heap keys | [Allocation log](compaction-delta-2026-11-07/allocation.log) |
| Existing persistence crash matrix | 89 checkpoints passed: bootstrap 21, PUT 4, rotation 15, compaction 24, multi-output build 14, rollback 5 and retirement 6 | [Crash matrix](compaction-delta-2026-11-07/legacy-crash-matrix.log) |

[Artifact checksums](compaction-delta-2026-11-07/artifacts.sha256) cover the retained logs, samples,
validated patch and source hashes.
Machine-readable [results](compaction-delta-2026-11-07/results.json) distinguish the passing isolated
campaign from the combined-workspace failure and the unproven latency/hosted/durability claims.

The six new integration cases include four durable configurations, entry/key/zero budgets,
successful retry after overflow, repeated erase/reinsert with binary keys, the official paired
Store path, seven I/O-fault classes and eight SIGKILL transition classes with acknowledged
post-snapshot PUT/ERASE operations. They verify live values, erase/TTL behavior, exact Index
accounting and reopen. Existing unrelated-rotation, commit-gate/depth and cold-read lifetime
controls remain in the full and sanitizer suites.

At a kill after private Manifest write, existing intent recovery may retain a recognized
`.manifest.glifi.tmp`; it is recovery-safe. The crash test additionally requires the following
ordinary reopen to clean that temporary while preserving acknowledged values and deletion.
The recovery protocol was not changed to strengthen this cleanup behavior.

## Controlled staging overlap

The maintenance benchmark adds operation/GET/PUT p99.9, delta replay/overflow, discarded staging
bytes, snapshot/gate maxima and separate definite-conflict retries. `--same-worker` with cooperative
mode releases foreground work after the compaction snapshot reaches its resource preflight, so
the experiment exercises concurrent staging rather than racing initial admission. Retry latency
is included in operation samples; only definite `sequence_conflict` results are additionally retried.

Common arguments:

```sh
glifistore_maintenance_benchmark --mode cooperative --same-worker \
  --operations 10000 --threads 2 --put-percent 10 --value-bytes 4096 \
  --warmup 0 --repeats 3 --cooldown-ms 0 --compaction-delta-entries N
```

| Journal events N | Successful compactions | Overflow aborts | Replayed events per successful attempt | Discarded staging bytes per attempt |
| --- | --- | --- | --- | --- |
| 0 | 0/3 | 3/3 | 0 | 32,515,776 |
| 4096 | 3/3 | 0/3 | 1006 | 0 |

Raw [disabled](compaction-delta-2026-11-07/delta-disabled.csv) and
[enabled](compaction-delta-2026-11-07/delta-enabled.csv) samples retain all latency and rotation
fields. These Debug smoke runs overlapped other validation on the same host. They demonstrate
successful reconciliation and reduced discarded staging, not improved tail latency: enabled PUT
p99.9 was higher in these samples, and the baseline completed no compactions. A controlled Release
campaign with adequate PUT sample counts and comparable reclaim progress is still required.

## Remaining limits

- Full Index snapshot enumeration remains O(N) under synchronization.
- Sustained writes can overflow the bounded journal; arbitrary-load reclaim progress is unproven.
- Worker publication contention, pending groups and any changed Manifest can still abort a job.
- The global intent/promotion/publication/retirement/audit lease remains the v1 protocol.
- Linux, FreeBSD and OpenBSD native/sanitizer rows, hostile-load p99.9 budgets and physical durability
  require fresh evidence. The existing prototype and durability claim ceilings remain unchanged.

Source checks: assurance catalog/schema and generated views, documentation links, structure debt,
claim honesty, changed-source clang-format and `git diff --check`. The workspace Python tooling suite
also passed 446 tests under Python 3.14; it included concurrent tooling changes outside this patch.
