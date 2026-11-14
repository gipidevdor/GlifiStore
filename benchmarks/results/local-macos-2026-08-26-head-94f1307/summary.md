# HEAD performance recon — paired engine (2026-08-26)

Status: lab evidence (architectural prototype)
Commit measured: `94f1307` (`codex/tcp-phase-attribution`)
Platform: Apple M4, macOS 26.6.2, AppleClang 21, `macos-release`
Claim ceiling: same-machine advisory numbers only — not capacity, E3/E4, or release gates.

Artifacts: this directory. Phase dump: `attribution-store-put-get.txt`.

## 1. Architecture as implemented (code, not docs)

Per shard (`ShardPairRuntime::Lane`):

| Role | Lifetime / ownership |
| --- | --- |
| Writer thread | `run(shard)` until `stop_and_drain`; owns `writer_generation`, merge, retired list |
| Reader | Host/Reactor or embedded caller; adopts `published_generation` raw pointer |
| Publication authority | Only Writer publishes (`publish_read_generation`); hosts never publish |
| Sync lane | LIFO→FIFO `SyncMutation*` under `sync_mutex`; caller waits on `done` |
| Async lane | Bounded SPSC + `MutationSlotPool` payload arena; completion sink |
| Read lease | `active_read_leases` + seq_cst fence before adopt; reclaim without wake-on-every-GET |

### GET (embedded volatile)

`Store::get_copy` → hash → route → `OperationGuard` → `ReadLease` → `PairReadGeneration::get`:

1. `active_read_leases++` + fence + `published_generation` acquire load
2. delta find → else base get
3. decode pin + `OwnedValue` copy
4. lease `--`; if last, set `reclaim_requested` (no Writer wake)

**Claim check:** after adopt, GET uses no mutex and no `shared_ptr` RMW on the generation.
It still pays two atomics on the lease and one acquire load on adopt — not “zero atomics”.

### PUT (embedded volatile)

`Store::put` → `mutate` → sync enqueue + wake → Writer:

- apply `put_volatile_published` (encode + Index)
- `publish_incremental` (new generation shell + delta COW) + `publish_read_generation`
- set `status` / `done`

**Linearization point (volatile):** Store Index publish, then Reader visibility at
`publish_read_generation` release store. ACK only after that publication for the item.

### Accorpamento pubblicazione (già presente)

| Path | Behavior |
| --- | --- |
| Sync volatile | Drain up to **32** `SyncMutation`, one `publish_incremental` (`kSyncPublicationBatch`) |
| `Store::put_batch` | Groups into Writer sync list → same ≤32 publication chunks |
| Async (TCP) | Coalesce up to `max_records` (default 32); durable_group may wait ≤`max_wait_ms` / 250µs burst |
| Sync durable_sync single-op | **One publication per item** (fsync-dominated) |
| Single `Store::put` | Always batch size 1: caller blocks until ACK before next enqueue |
| Reactor GET (post `adopt_read_generation`) | Raw generation pointer for the turn — **no** `ReadLease`, no generation `shared_ptr` bump |
| Connection-level PUT coalesce until GET barrier | **Proposed only** (`docs/benchmarks/paired-shards-plan.md`); not implemented |

## 2. Baseline (median)

### Embedded volatile (1 Worker, value 64 B unless noted)

| Workload | ops/s | ns/op |
| --- | ---: | ---: |
| GET | 3.87 M | 259 |
| PUT | 352 k | 2837 |
| PUT batch (32) | 522 k | 1916 |
| PUT+GET | 647 k | 1545 |
| RAW | 623 k | 1606 |
| PUT parallel 4w affine | 286 k | 3495 |
| GET parallel 4w affine | 14.4 M | 70 |

PUT batch vs single PUT: **+48% throughput** at fixed correctness (FIFO, one ACK after visibility per item, not a cross-key transaction).

### Payload sweep (PUT 1w)

| value | ops/s | ns/op |
| --- | ---: | ---: |
| 16 B | 421 k | 2376 |
| 64 B | 352 k | 2837 |
| 256 B | 367 k | 2722 |
| 1 KiB | 367 k | 2728 |
| 4 KiB | 278 k | 3600 |
| 16 KiB | 34 k | 29562 |

Small payloads are handoff/publication bound; large payloads become copy/RSS bound.

### TCP volatile RAW (1w/1c)

| pipeline | ops/s | ns/op (pair) |
| --- | ---: | ---: |
| 1 | 44 k | 22934 |
| 8 | 107 k | 9362 |
| 32 | 240 k | 4160 |
| 128 | 113 k | 8823 |

Embedded PUT (~352 k) ≫ TCP p1 (~44 k). Pipeline 32 is the sweet spot on this host;
p128 regresses vs p32 (buffering / completion / backpressure — needs dedicated TCP phase dump).

## 3. Phase attribution (PUT, phases ON)

From `store-put-get` instrumented run (timer overhead in absolute ns; use %):

| Phase | mean ns | share of PUT |
| --- | ---: | ---: |
| `ack` (caller wait) | 2749 | **66.6%** |
| `publish` | 596 | **14.4%** |
| `worker_apply` | 419 | 10.2% |
| `index_publish` | 146 | 3.5% |
| `enqueue` | 102 | 2.5% |
| `encode_copy` | 52 | 1.3% |
| `admit` | 32 | 1.6% |

**Hypothesis verdict:** publication is a major **Writer-side** cost (~59% of apply+publish),
but the dominant end-to-end PUT cost is **thread handoff / ack wait**, not Index.
Index publish is only ~3.5% of PUT.

GET attribution (after wiring `GS_PHASE_GET(index_lookup)` in `PairReadGeneration::get`):

| Phase | mean ns | share |
| --- | ---: | ---: |
| `index_lookup` (+ decode) | 265 | **68%** |
| `lease_adopt` / `hash` / `route` / `admit` | ~31 each | ~8% each |
| `value_copy` | 36 | ~0.2% |

Matches the architectural story: after adopt, lookup dominates; lease atomics are
visible but secondary. (Seed PUTs during the GET harness also emit PUT phases.)

## 4. Accorpamento adattivo — decision

Ideal policy requested (empty queue → immediate; depth → larger batch ≤32) is **already
how sync volatile works** when multiple mutations are queued. What single-threaded
`Store::put` cannot do without **raising latency** is wait for more work: the caller
holds the critical path until ACK.

| Proposal | Verdict |
| --- | --- |
| Add sleep/yield waits to fill publication batches on single PUT | **Reject** — trades p50/p99 for ops/s against priority order |
| Prefer `put_batch` / TCP pipeline / async coalesce for throughput | **Accept** — existing contract |
| Land ADR 0036 generation slot-pool to cut publish alloc | **Open** — design bar + gates; not silent land |
| Busy-spin Writer after sync (historical) | **Reject** — prior lab −50% `store_put` |

Point of linearization for a publication batch of A,B,C: each mutation is linearized
in FIFO Store order; Reader sees them together only after one generation publish.
**Not** a multi-key transaction.

## 5. Atomiche

**Correttezza (retain):** `published_generation`, epochs, admission, `signal`,
`healthy_`/`expire_remaining_`, lease count, SPSC indices, sink completion.

**Osservative:** lane histograms / max counters / merge stats — candidates for
Writer-local + periodic publish in a later slice; do not strip assurance counters
without an alternate evidence path.

False-sharing: hot pointers already `alignas(128)` (`published_generation`,
`writer_epoch`, `reader_safe_epoch`, sync admission vs lease counters).

## 6. Next justified work (ordered)

1. TCP phase dump on p32 vs p128 (why p128 loses) — syscall / encode / completion resume.
2. Keep Index untouched until profiling shows it again.
3. ADR 0036 only behind V1–V14 gates.
4. Optional: Writer-local observational counters A/B under TSan.
5. Do **not** add adaptive waits on the single-op sync PUT path.

## 7. Environment note

`glifistore_benchmarks` from `macos-release` may still print a stale
`# git_sha=` from CMake cache; objects were rebuilt against `94f1307`.
Phases binary correctly reported `git_sha=94f1307`.
