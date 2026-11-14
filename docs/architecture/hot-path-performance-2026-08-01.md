# Hot-path performance program

**Historical** · embedded Store GET/PUT and TCP daemon hot paths on macOS Apple Silicon · 2026-08-01

Claim ceiling: architectural prototype. The numbers below are same-machine lab evidence on
Apple M4 / `macos-release`; they are not production capacity claims, E3 durability closure, or
hostile-public deployment readiness.

The 2026-08-01 program to reduce the gap between the internal Index (~11–16 M ops/s) and observable
Store API and TCP daemon performance, without weakening correctness, durability, recovery,
backpressure, fail-closed behaviour, RAW semantics or memory bounds. Later entries run to
2026-08-27; rejected ideas are also listed in
[rejected hot-path optimizations](hot-path-rejected-optimizations.md).

Raw results:

- baseline reference (`629bc68`):
  [`benchmarks/results/local-macos-2026-08-01/`](../../benchmarks/results/local-macos-2026-08-01/);
- candidate measurements:
  [`benchmarks/results/local-macos-2026-08-01-perf/`](../../benchmarks/results/local-macos-2026-08-01-perf/);
- HEAD recon (2026-08-26, `94f1307`):
  [`benchmarks/results/local-macos-2026-08-26-head-94f1307/`](../../benchmarks/results/local-macos-2026-08-26-head-94f1307/);
- complete paired/runtime rerun (2026-08-26, dirty tree based on `fae4547`):
  [`benchmarks/results/local-macos-2026-08-26-full-fae4547-dirty-rerun/`](../../benchmarks/results/local-macos-2026-08-26-full-fae4547-dirty-rerun/).

## Phase instrumentation

Optional compile-time counters (`-DGLIFISTORE_HOT_PATH_PHASES=ON`):

- header: `include/glifistore/core/hot_path_phases.hpp`;
- preallocated `alignas(64)` atomic buckets; no global mutex; no hot-path logging;
- dump via `GLIFISTORE_HOT_PATH_PHASE_REPORT=1` at process exit;
- default builds keep the macros as no-ops;
- TCP reports only additive leaf spans: decode, route, store operation, response encode, transport
  write call and poller update. In particular, `socket_write` does not include pipeline resumption
  or request dispatch.

### Cost map

Measured on `store-put-get` (1 worker, 200k ops, 5 repeats) with instrumentation enabled. Absolute
nanoseconds include timer overhead, so attribution uses percentages.

| Path | Phase | Share | Notes |
| --- | --- | ---: | --- |
| GET | `index_lookup` (+ decode) | ~67% | Delta/base Swiss probe + pin decode |
| GET | `hash` / `route` / `admit` / `lease_adopt` | ~8% each | Admission + ReadLease fence |
| GET | `value_copy` | small after SSO | 64 B inline path |
| PUT | `ack` | ~93% | Caller wait for Writer apply+publish |
| PUT | `enqueue` | ~4% | Sync list push + wake |
| PUT | `admit` | ~3% | Submission admission |
| TCP | `socket_write` | ~76% | `send` / `sendmsg` / TLS write call only |
| TCP | `store_op` | ~11% | Local GET or mutation submission; excludes response encode |
| TCP | `poller_update` | ~10% | kqueue/epoll interest modification |
| TCP | decode / route / encode | ~3% | Additive remainder of instrumented leaf spans |

The TCP shares come from a same-machine Apple Silicon lab run with phase instrumentation enabled:
volatile read-after-write, one Worker, one client, pipeline 128, 200k PUT+GET pairs, two warmups and
seven measured repeats. They are diagnostic attribution, not release evidence or a production claim.

## TCP changes (2026-08-25)

### Redundant poller-transition removal

Async store submission used to modify poller interest inside `execute_local`. Both callers then
reconciled that interest before any intervening poller wait: `read_ready` immediately flushed queued
output, while `write_ready` performed its own final transition. Interest ownership now stays with
those callers. No queue, acknowledgement point, request ordering, output watermark or wire outcome
changed.

In the instrumented pipeline-128 workload, poller-update samples fell from about 3.59M to 1.79M
across warmups and measured runs. A non-instrumented Release A/B, alternated to reduce order bias,
measured:

| Run | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| A | 228,212 ops/s | 241,696 ops/s | +5.9% |
| B (reverse order) | 229,004 ops/s | 243,526 ops/s | +6.3% |

Pipeline 1 stayed effectively flat in a shorter check (47,011 vs 47,278 ops/s, +0.6%). These local
figures are advisory and keep the same-machine, non-isolated benchmark limitations.

A second same-state transition remained after a synchronous output flush. When `write_ready` is
entered from `read_ready`, drains the queued response and has never armed writable interest, an
in-flight request leaves the socket already registered for reads. Skipping that redundant
`read → read` modification is behavior-neutral; half-closed and write-armed sockets still take the
complete reconciliation path.

With phase instrumentation, this reduced poller-update samples from about 1.79M to zero in the same
pipeline-128 workload. A Release A/B against `fe32cfc`, alternated to reduce order bias, measured:

| Run | `fe32cfc` | Candidate | Difference |
| --- | ---: | ---: | ---: |
| A | 242,112 ops/s | 268,755 ops/s | +11.0% |
| B (reverse order) | 243,485 ops/s | 272,709 ops/s | +12.0% |

Pipeline 1 stayed flat in a shorter check (47,606 vs 47,755 ops/s, +0.3%). The figures remain
advisory same-machine evidence, not a release claim.

### Completion resume with Writer/socket overlap

The mutation completion path now resumes already-buffered frames before draining its contiguous
decided output. `process_frames` still admits at most one new asynchronous operation on the
connection, so the next Writer request can overlap the socket write of prior ordered responses
without a response reorder buffer, queue widening or early acknowledgement. A trailing decode or
admission failure preserves prior decided responses through `close_after_flush`. Resume is
suppressed when output was already pending at completion, keeping the historical slow-client
backpressure boundary.

In the instrumented pipeline-128 workload, socket-write samples fell from about 3.60M to 1.80M while
poller-update samples stayed at zero. A Release A/B against `dbf246d`, alternated to reduce order
bias, measured under the same host state:

| Run | `dbf246d` | Candidate | Difference |
| --- | ---: | ---: | ---: |
| A | 197,994 ops/s | 306,616 ops/s | +54.9% |
| B (reverse order) | 200,705 ops/s | 309,928 ops/s | +54.4% |

An earlier candidate reached 419,101 ops/s under a less contended host state; after the explicit
pending-output backpressure guard was added, a fresh seven-sample Release run measured 423,376 ops/s
median. Neither absolute value is used for the A/B ratio. Pipeline 1 showed no regression in the
sampled check. All figures remain advisory same-machine evidence.

## Hot-path diagrams

```text
GET (embedded Store)
  hash(key) → route(shard) → OperationGuard(admit)
    → ReadLease (count + seq_cst fence + load published generation)
      → PairReadGeneration::get
           delta.find → (miss) base.get
           decode pinned record → OwnedValue (SSO ≤64 B)
    → lease drop (no Writer wake)

PUT (embedded Store)
  hash → route → OperationGuard → maintenance check
    → ShardPairRuntime::mutate
         enqueue SyncMutation → wake Writer
         adaptive spin → park on done
    Writer: apply Store mutation → publish_incremental → proportional reclaim
         → notify done (ACK). No early ACK before publication.
```

```text
TCP daemon (owner-bound)
  Reactor turn: clock once → decode → authz/abuse → dispatch
    → local generation GET / async mutation lane
    → encode response (preallocated connection output) → write
  Connections are BIND_WORKER / owner-bound; SDKs open one connection per Worker.
```

## Concurrency invariants

The program left these invariants unchanged:

1. RAW: a completed mutation happens-before any later read that observes it; ACK is only after
   Writer publication.
2. No reclaim on GET: ReadLease no longer wakes the Writer; reclaim is Writer-opportunistic and
   proportional to retired-generation debt.
3. Admission closed bit: OperationGuard and submission counters remain fail-closed for `close()`.
4. Bounded memory: mutation slots, queues, retired-generation cap and connection watermarks are
   unchanged; no queue widening.
5. Hash and routing: a single `PrehashedKey` / `HashedKey` computation per public operation; span
   key overloads converge on `KeyView`.

## Results (lab medians)

| Cell | Baseline (`629bc68`) | After (this program) | Ratio |
| --- | ---: | ---: | ---: |
| Index find hit (ref) | 11.59 M ops/s | (unchanged suite) | — |
| `store_get_copy` 64 B 1t | 2.20 M (455 ns) | 3.50 M (286 ns) (15 repeats) | 1.59× |
| worker-affine GET 2t | 5.07 M | 8.46 M | 1.67× |
| `store_put` 64 B 1t | 378 k | ~369 k | ~0.98× |
| worker-affine PUT 2t | 506 k | ~504 k | ~1.00× |
| uniform PUT 2t | 140 k | ~241 k | 1.72× |
| TCP w1 p1 | 47.6 k | 46.5 k | ~0.98× |
| TCP w4 p32 | 282 k | 302 k | 1.07× |
| TCP w4 p128 | 206 k | 312 k | 1.51× |
| TCP w4 p32 / w1 p1 | 5.91× | 6.50× | target ≥2.5× |

| Target | Result |
| --- | --- |
| get_copy 64 B 1t stably < 300 ns (≥3.3 M) | Met (median 286 ns / 3.50 M over 15 repeats) |
| worker-affine GET 2t ≥ 6 M | Met (~8.5 M) |
| volatile PUT 1t ≥ 600 k | Not met (~369 k); residual is Writer ack/publish |
| worker-affine PUT 2t ≥ 900 k | Not met (~504 k); same residual |
| uniform PUT 2t ≥ single-thread PUT | Not met (~241 k vs ~369 k); cross-shard sync still serializes per owner |
| TCP p32 4-pair ≥ 2.5× 1-pair | Met (~6.5×) |
| p128 not regress >5% vs p32 | Met (w4 p128 ≥ p32 in this run) |

## Changes landed

1. Disableable hot-path phase counters and an exit report.
2. GET: span→`KeyView` convergence; `PrehashedKey` alias; ReadLease no Writer wake on last lease
   drop; `OwnedBytes` SSO ≤64 B; Lane false-sharing separation for lease versus sync atomics.
3. PUT: bounded adaptive spin before park; Writer short spin before park; proportional reclaim (skip
   empty or non-quiescent; quantum reclaim).
4. TCP: additive leaf phase scopes; clock comment and a single sample per `process_frames` turn;
   encode, socket-write and poller-update scopes. The p128 improvement is attributed to reduced
   GET↔Writer wake storms under pipelined load plus encode/write path hygiene, not to queue
   widening.
5. TCP: removed redundant async-submission poller modifications; the read/write caller remains the
   sole owner of the next interest transition.
6. TCP: skip the remaining same-state read-interest update after a synchronous flush that never
   armed writable interest; half-close and writable states still reconcile through the poller.
7. TCP: resume bounded buffered work after mutation completion before draining contiguous output,
   preserving one-in-flight response order while overlapping the next Writer operation with the
   socket write.

## Rejected optimizations

| Idea | Reason rejected |
| --- | --- |
| Widen mutation / TCP queues | Violates bounded-memory / backpressure non-negotiable |
| Remove length/overflow checks | Safety ban |
| Early ACK before publication | Breaks RAW |
| Drop ReadLease fence / counts without epoch protocol | Use-after-free hazard vs Writer reclaim |
| Replace production `shared_ptr` generation publish with prototype slot pool | Requires ADR + proofs; not silent |
| Disable OperationGuard on GET | Breaks close/admission linearization |
| Game benchmarks / loosen thresholds | Explicitly forbidden |
| Dual base/candidate worktrees | Out of scope for this execution |

## Memory accounting

Fixed costs dominate small-value RSS in these benchmarks (Segment arenas, paired Writer threads,
generation pins). `store_get_copy` median RSS is ~240 MiB with ~0 delta during the measured loop, so
capacity is not "bytes per key" for hot 64 B values. Segment size and per-shard Writer stacks are
structural; see the [storage model](storage-model.md) and paired ADR 0031/0032.

The 2026-08-27 memory block replaced the immutable base's per-bucket
`control + uint64 hash + record pointer` payload (17 bytes) with `control + uint32 record index`
(5 bytes). The full hash moved into the existing 64-byte compact record, and every candidate still
receives a full-hash and full-key comparison. At 200,000 16-byte keys the base capacity was 262,144
buckets: the exact lookup payload fell from the 4,456,448-byte counterfactual to 1,310,720 bytes,
saving 3,145,728 bytes (70.59% of base lookup metadata) without a disk, wire or visibility change.

The same block added a coherent current-generation allocation-payload lower bound and a dedicated
native allocator/RSS census. On the local Apple arm64 run, current live structures accounted for
roughly 59 MiB. Adding full Segment reservation explained the native allocator's current in-use
bytes to within 0.33 MiB (about 99 MiB for one shard and 301 MiB for four); fixed 64 MiB Segment
capacity is therefore the dominant virtual reservation at higher shard counts. Process RSS stayed
about 212 MiB with one shard and 103 MiB with four shards, because untouched Segment pages are lazy
while transient publication and merge pages can remain resident. Only one retired generation per
shard remained and no merge was active at sampling time. This evidence narrows the open problem but
does **not** close total memory amplification. Reproducible commands and raw cells are under
`benchmarks/results/local-macos-2026-08-27-read-generation-memory/`.

Allocation-event inspection then identified the one-shard residual: the macOS zone retained every
successive 0.5 MiB immutable-record-array merge size, including 133.5 MiB of empty large regions.
Large record arrays now use guarded anonymous mappings in geometric size classes, with one spare
owned by the immutable-base lineage. Same-class merges alternate buffers; class changes and lineage
destruction unmap the old spare. On the same dirty local source, the 200,000-entry one-shard census
fell from 212.25 MiB RSS to 92.83 MiB (−56.3%); four-shard RSS fell from 103.51 MiB to 93.31 MiB.
An interleaved same-source diagnostic A/B placed five-repeat `store_put_batch` medians around
594–610 kops/s for general-allocation baselines and 594–610 kops/s for the bounded mapping pool,
while immediate unmap measured only 550–564 kops/s. These are local directional results, not a
release claim, but they justify the bounded reuse design over both unbounded allocator retention and
unmap-on-every-retirement.

The first coherent census implementation accidentally made every publication walk the complete
reachable delta directory. A macOS CPU sample attributed 65.3% of all samples to that accounting
path, reducing a 200,000-item single PUT cell to 122.7 kops/s. The Writer now maintains exact
reachable page, block and chunk counts while creating COW nodes, and the immutable base tracks
allocated key-block bytes as they are reserved. Census export is O(1): the matched local cell
reached 522.2 kops/s (+325.6%) at unchanged RSS, and the accounting path fell to 0.13% of sampled
stacks. No publication, ACK, routing or GET ownership rule changed. Raw local evidence and
limitations are under `benchmarks/results/local-macos-2026-08-27-put-census-o1/`.

## Residuals and follow-ups

- Current local single `Store::put` is ~1.92–2.03 µs median for 16–64 B values after removing the
  accidental O(n) census. The remaining cost is real publication COW, proportional merge work and
  generation retirement. The nested Writer `OperationGuard` on the embedded sync path stays removed
  through `PublishedAdmission::caller_holds_guard`; the maintenance emergency gate is still checked.
- Incremental merge scheduling now accounts exact remaining scan slots against the remaining bounded
  post-cut entry and record-version capacity before every publication. A single PUT pays one
  configured minimum quantum; a coalesced batch amortizes two, and the proportional term can exceed
  that floor only when required to avoid exhausting the post delta. Dedicated idle quanta run after
  completion delivery and release the execution token between turns. This removes duplicated
  combiner housekeeping and the latent 256-quantum terminal cliff without changing RAW or ACK.
  Same-shape local checks improved within modest host variance: 506,244 → 519,513 PUT/s single
  (+2.62%; baseline 5, candidate 9 samples) and 670,405 → 676,662 PUT/s batch 32 (+0.93%; baseline
  5, candidate 9 samples); the 1.5M growth run went from 182,632 to 186,112 PUT/s (+1.91%). A
  separate temporary three-quantum control (the same final code except the two historical duplicate
  calls restored) over one million timed PUTs measured p99.9 236.7 → 92.8 µs (−60.8%), p99
  3.04 → 2.63 µs (−13.7%), and instrumented throughput 482,574 → 522,026 PUT/s (+8.2%). These are
  same-host advisory figures, not release evidence or a cross-platform tail claim.
- `Store::put_batch` with Writer sync coalescing (≤32 per publication) amortizes publication when
  the caller stages several same-shard mutations in one call. Current local medians are ~672 kops/s
  (batch 32) versus ~493 kops/s for single PUT on Apple M4, a gain that leaves single-operation
  semantics unchanged.
- ADR 0035 (rejected): a TLS shell freelist under the existing publish protocol gave a mild 1t PUT
  gain and −16% affine PUT 2t on A/B; it was not shipped. The residual remains Delta COW spine plus
  Writer apply, not shell malloc alone.
- Extend the allocation census to uniquely attribute retired and shared generation nodes,
  in-progress merge builders, thread stacks, Segment resident pages and the remaining 4/8 MiB native
  allocator regions. Current and spare immutable-base mapping payloads are now explicit;
  steady-state and cross-platform high-water evidence remains open.
- `make_shared` co-allocation for generation shells, then `DeltaState` embedded in the same
  allocation: lab `store_put` ~370–378 k (versus ~344 k plain the same day). Affine stays within
  prior interleaved noise, still far from the 600 k single-operation target.
- The Writer sync volatile path attributes `worker_apply` versus `publish` when
  `GLIFISTORE_HOT_PATH_PHASES` is on. Lab: apply ~381 ns, publish ~640 ns, caller `ack` ~3.0 µs
  (wake and scheduling dominate the residual). Rejected: post-sync Writer busy-spin (−50%
  `store_put`), sync-before-merge reorder (no reliable 1t win), and conditional `writer_waiting`
  notify (A/B within noise; sync PUT leaves the Writer parked, so notify still fires). See
  `benchmarks/results/local-macos-2026-08-02-sync-first/` and
  `benchmarks/results/local-macos-2026-08-02-writer-waiting/`.
- DeltaArena keeps Segment pins in a `deque<SegmentPtr>` (stable raw addresses, no per-pin
  `unique_ptr` shell). Moving `SegmentPtr` out of `ReadMutation` into the arena was considered and
  deferred: `publish_incremental` may be retried after `resource_exhausted`, and a consuming move
  would break pin validation on retry unless publications were rebuilt.
- Apply sub-phases are wired: `encode_copy` (Segment append) and `index_publish`
  (`Worker::publish`). Lab means: encode ~66 ns, index ~158 ns, outer `worker_apply` ~514 ns,
  `publish` ~603 ns, `ack` ~2.7 µs (~64%). The redundant second `encoded_record_size` inside encode
  was removed when the caller already sized the buffer (`encode_record(out, input, size)`). Encode
  is not the binding limit.
- A thread-local Delta COW freelist was measured and rejected: allocator and custom-deleter overhead
  regressed uniform parallel PUT on Apple Silicon.
- Uniform multi-thread single-operation PUT is still below 1t PUT: combine `put_batch` with
  caller-side group-by-owner, or a future shard-bound session.
- The full generation slot-pool publish protocol is ADR 0036 (proposed): its design bar and V1–V14
  matrix are in [ADR 0036](../adr/0036-generation-slot-pool-publish.md). Production stays on
  `shared_ptr` + ReadLease until that ADR is accepted and its gates close. The reference remains
  `src/experimental/paired_shard.cpp` (lab only).
  - Prototype evidence covers V1/V2/V3/V6/V7/V9/V13: bounded slot acquire → `resource_exhausted`; a
    rejected publication is never visible; `macos-tsan` and `macos-asan` ASan+UBSan green on the ADR
    gates and the paired Store.
  - Production-baseline notes for V3/V4/V6/V8/V10/V14 are under
    `benchmarks/results/local-macos-2026-08-02-adr0036-v6-v8-v14/`,
    `benchmarks/results/local-macos-2026-08-02-adr0036-v6-failclosed/` and
    `benchmarks/results/local-macos-2026-08-02-adr0036-v14-sync-matrix/` (91 sync crash
    checkpoints).
  - The V6 production seam also covers async and sync durable sibling drain-snapshot after a
    later-item failure, including `allow_fail_closed` when durable already self-closed. It rejects a
    success ACK for abandoned unflushed writer-batch siblings (`durable_through` gate on commit
    failure and unhealthy no-op; flushed siblings keep ACK and snapshot), sticky-fails and stops
    Writer batches on a deferred-TTL drain that is indeterminate, preserves staged async sibling
    success ACKs across a Writer catch, and keeps immutable published-generation `Store::get` RAW
    after sticky fail-closed (mutable Index GET and mutations are still rejected).
  - Sync `durable_sync` single-operation also drain-snapshots before sticky close on a post-commit
    publication failure. Happy-path incremental publish failure also drains before sticky close.
  - Async durable ACK-after-drain clears capture-fail errors on clean commits once the snapshot is
    published (no GET-visible mutation with an error ACK).
  - Sync single-operation gives ACK-after-visibility for committed+error after drain and preserves
    success across a catch after publish. Sync batch and async durable extend the same visibility
    upgrade to sticky Index-visible `committed+error`; the catalog advances `durable_through` before
    secondary Index accounting (`Site::index_account`).
  - Sync durable batch never success-ACKs unprocessed items after a catch-path drain (sticky
    Index-committed only, never a presence upgrade of attempted puts on pre-existing keys).
  - Index and hot flush publication catches allocation throws into fail-closed (clears pending).
  - Volatile sync preserves the success ACK across a catch after publish (put and erase); the async
    Writer acknowledges after publish for staged indices and preserves completions across a
    post-publish catch (put and erase).
  - Sync durable catch keeps the resolved error polarity (no success promotion).
  - `writer_durable_through` locks the Worker mutex for finalize gating.
  - Evidence is under the paired durable litmus and the allocation-fault suite.
