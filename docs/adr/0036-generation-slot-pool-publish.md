# ADR 0036: Generation slot-pool publish protocol — design bar before production

- Status: proposed
- Date: 2026-08-02
- Deciders: storage and performance maintainers
- Applies to: paired `PairReadGeneration` publication and reclaim (embedded Store and
  daemon Reader–Writer pairs); **Wave 1 opt-in production path exists behind
  `PairedConcurrencyConfig::generation_slot_pool` (default false)**; Alternative A remains
  the bit-identical default
- Amends: none yet (would amend [ADR 0031](paired-reader-writer-shards.md) §Protocollo di
  publication / §Protocollo di reclamation **only after** the verification matrix below is
  closed and this ADR is accepted)
- Supersedes: none
- Depends on: [ADR 0031](paired-reader-writer-shards.md), [ADR 0032](0032-paired-concurrency-embedded-store.md)
- Related: [ADR 0035](0035-generation-shell-recycling.md) (rejected shell freelist under the
  *existing* protocol; Alternative A deferred here)

## Context

Production publish keeps Writer-owned `shared_ptr<const PairReadGeneration>` (today co-allocated
with embedded `DeltaState` via `make_shared`), exposes a raw pointer to Readers, and reclaims via
ReadLease / `reader_safe_epoch` QSBR ([ADR 0031](paired-reader-writer-shards.md)). Lab attribution
on Apple Silicon shows single-op PUT dominated by caller `ack` (~64%, wake/schedule) then
`publish_incremental` (~600 ns) and Writer apply (~500 ns). Encode is ~66 ns and is not the binding
limit (`benchmarks/results/local-macos-2026-08-02-encode-size/`).

ADR 0035 rejected recycling generation *shells* under the current protocol after a **−16%**
worker-affine PUT 2t regression. The remaining publish lever that removes `shared_ptr` control-block
traffic is a **fixed generation slot pool** with a different lifetime protocol. A lab prototype
lives in `src/experimental/paired_shard.cpp` (capacity ≈ mutation queue + 2, reader-turn QSBR,
per-slot `output_pins`). That prototype is **not** promotable: it is not the production delta/base
model, lacks durable refresh/merge integration, and has not closed ADR 0031’s proof matrix.

This ADR freezes the **design bar** and **acceptance gates** before any production integration.
No silent protocol swap; no partial land that weakens RAW, lease safety, or reclaim bounds.

## Decision drivers

- **RAW**: ACK only after the mutation is in the published generation (release-store).
- **No generation UAF**: concurrent GET / cold I/O / scatter output must not observe freed storage.
- **Atomic adoption**: Readers never see torn epoch vs generation (single release descriptor).
- **Bounded memory**: fixed pool capacity; exhaust → backpressure, never unbounded retire growth.
- **ADR 0031 compatibility**: incremental merge, durable catalog refresh, shutdown drain, fail-closed
  publication failure after Store mutation remain intact or are explicitly re-specified.
- **Measured non-regression**: especially worker-affine parallel PUT (ADR 0035 lesson).
- **No silent format / wire / routing change**.

## Alternatives considered

### A. Keep production `shared_ptr` + ReadLease reclaim (status quo)

- Advantage: proven under ADR 0031; embed-delta / `make_shared` already reduce one alloc per publish.
- Disadvantage: control-block and COW spine cost remain on every single-op publish.
- Remains the **production default** until this ADR is accepted *and* verification closes.

### B. TLS / freelist shell recycle under current protocol (ADR 0035)

- Rejected after measured affine PUT regression. Not reopened here.

### C. Full generation slot pool (this ADR’s subject)

Fixed slots; Writer publishes by filling a free slot and release-storing a descriptor; reclaim waits
for a proven quiescent boundary (reader turns and/or lease/epoch frontier) plus zero outstanding
pins that borrow generation storage.

- Advantage: removes per-publish `shared_ptr` generation control blocks; bounds live generations.
- Disadvantage: new reclaim protocol; must prove equivalence (or a strict strengthening) of ADR 0031
  lease/epoch rules for embedded Store, daemon GET, cold I/O, and future borrowed iovec GET.

### D. Hybrid: slot-pool shells, keep ReadLease counters

Map slots to the existing lease fence without reader-turn QSBR.

- Advantage: smaller delta from production Reader code.
- Disadvantage: still need a precise “slot free only when…” rule; easy to get wrong if leases and
  slots disagree. Allowed as an *implementation strategy* only if it satisfies the same proof matrix.

## Decision

**Proposed design bar (not yet production law):**

1. Production must not adopt the experimental slot-pool publish path until this ADR is **accepted**
   and the Verification matrix is marked closed with linked evidence.
2. Any production slot-pool must preserve:
   - single atomic publication of `{epoch, visible_through, generation graph}`;
   - ACK-after-publication RAW;
   - bounded pool + explicit backpressure when no free slot exists after reclaim;
   - fail-closed behavior when Store mutation succeeded but publication cannot complete;
   - shutdown drain that never frees a generation still observable by a Reader or pin.
3. Reclaim must define a **single** quiescence story that covers at least:
   - embedded `ReadLease` (or an explicitly documented replacement with the same happens-before);
   - daemon per-turn adoption;
   - async cold-read / `reader_safe_epoch` (or pin equivalent);
   - any output path that borrows generation bytes (today’s owning `OwnedValue` may release early;
     a future borrowed `get-into` must extend the frontier — same rule as ADR 0031).
4. Capacity: document the formula (e.g. related to max retired debt, mutation queue depth, and
   in-flight pins). Exhaustion is backpressure or fail-closed admission — never silent overwrite.
5. Incremental merge and durable snapshot replace remain Writer-private; slot reuse must not hand a
   Reader a generation whose delta/base arenas were reset in place without a new epoch.
6. Claim ceiling for benches remains architectural prototype / lab evidence until E3 durability
   closure says otherwise.

**Until acceptance: keep Alternative A in production.** Implementation work may proceed only behind
compile-time / runtime guards that do not change default paired Store behavior, or as isolated
prototype/tests that do not link into `glifistored` defaults.

## Consequences

### Positive (if accepted and shipped later)

- Clear path to remove generation `shared_ptr` churn from `publish_incremental`.
- Fixed upper bound on concurrent live generations per lane.
- Forces explicit pin accounting for borrowed reads before they exist.

### Negative / risks

- Large patch surface in `shard_pair_runtime` and Reader adoption.
- High risk of subtle UAF or lost-wakeup reclaim bugs; TSAN alone is necessary but not sufficient.
- May not move single-op PUT to the 600 k ops/s target if wake/schedule remains ~2.7 µs of `ack`.
- Affine parallel PUT must be gated; ADR 0035 showed “allocator win” can reverse under contention.

### Deferred

- Production code landing.
- Changing default reclaim from ReadLease to reader-turn QSBR (only with proofs).
- Any freelist of DeltaPage/Block/Chunk (already rejected separately).

## Compatibility and migration

- **Disk / wire / routing:** no change.
- **Public API:** no change required for `Store::put` / `get` semantics; internal publish/reclaim only.
- **Embedded vs daemon:** one protocol for both, or an explicit documented dual-mode with identical
  RAW and UAF guarantees (dual-mode discouraged unless proofs share the same matrix).
- **Migration:** feature flag or build flag with default **off** until soak; no silent flip on upgrade.

## Verification

The optional production slot path uses an empty-owner aliasing `shared_ptr` only as an observer
of pool-owned generations. Creating or mirroring that observer allocates no control block, grants
no ownership, and does not extend a generation's lifetime. Existing leases, pool pins and quiescence
remain authoritative. The [allocation-fault regression](../../tests/quality/allocation_fault_client_tests.cpp)
forbids allocations across observer creation and repeated publication mirroring; the
[2026-11-07 local campaign](../../engineering/evidence/cpp-correctness-local-2026-11-07.md) records
the result. This repair does not accept the ADR or enable the flag by default.

All items are **required** before accepting this ADR as *production-implemented*.
Prototype evidence closes lab gates only; production still uses Alternative A.

| # | Gate | Evidence required | Status |
| --- | --- | --- | --- |
| V1 | Litmus: publish release ↔ Reader acquire adopts one immutable generation | Unit / model or formal note + test | **production-partial** — production `GenerationSlotPool` V1 token adopt + ≥10k reincarnation (`tests/unit/generation_slot_pool_production_tests.cpp`); prior prototype-evidence retained. Default path remains Alternative A |
| V2 | Litmus: reclaim never frees epoch ≥ any active lease / pin / safe epoch | TSAN stress + fault injection | **prototype-evidence** — `ADR 0036 V2…` + V13 stress under `macos-tsan`; production TSAN soak still open |
| V3 | Two-boundary (or lease-equivalent) race: Reader mid-GET during publish+retire | Dedicated race test + TSAN | **prototype-evidence** — `ADR 0036 V3…` + V13 under `macos-tsan`; **production-baseline** — overwrite-storm under `macos-tsan` (`…/local-macos-2026-08-02-adr0036-v6-failclosed/`) |
| V4 | Cold I/O / `reader_safe_epoch` (or pin) holds reclaim across async completion | Integration | **prototype-evidence** (pin) — `paired slow-output pin…`; **production-baseline** — `paired Reader refreshes compacted durable pins…` + `durable cold read pin survives…` (`…/local-macos-2026-08-02-adr0036-v6-failclosed/`) |
| V5 | Shutdown drain: no UAF after stop; late mutations linearized or rejected fail-closed | Paired Store close tests | **production-local / partial** — opt-in slot pool V5 shutdown lease test plus V7 terminal-close litmus prove that, after Writer join and Reader-lease validation, an unfinished Writer-only merge releases its cut pin before final reclaim. Focused Release/ASan+UBSan/TSan local rows pass; full daemon shutdown under the flag and multi-OS evidence remain open |
| V6 | Publication failure after Store mutation remains fail-closed | Existing paired fail-closed tests + new slot-exhaust case | **production-partial** — production pool unit proves reserve-before-mutate + abandoned linearized guard fail-closed; runtime integration under flag for all Writer sites still open. Existing production baseline remains allocation-fault + paired durable litmus |
| V7 | Incremental merge + post-cut publish under slot pressure | Pair read generation + Writer merge tests | **production-local** — opt-in embedded combiner and dedicated Writer litmus publish cut/post-cut keys while the Reader frontier holds slot pressure, then resume and close cleanly (`ADR 0036 production slot V7…`). Focused Debug/Release/ASan+UBSan/TSan pass locally; long and multi-OS campaigns remain open |
| V8 | Durable catalog refresh / rotation does not invalidate in-flight GET | Paired durable refresh tests | **candidate-evidence** — real `PairReadGeneration` slot candidate preserves a cold Segment borrow across compaction refresh and publishes a Writer-owned rotation atomically; Release/ASan+UBSan/TSan local record. Production integration remains open |
| V9 | Pool exhaustion applies backpressure (no overwrite, no unbounded wait without admission bound) | Unit + overload | **production-partial** — production pool unit saturates 65 slots and recovers after frontier reclaim; Alternative A admission baseline retained. Full runtime overload under flag still open |
| V10 | Same-key FIFO within `put_batch`; RAW after ACK | Existing paired Store batch tests | **production-partial** — opt-in slot pool FIFO test (`ADR 0036 production slot V10…`); status-quo path remains production-baseline |
| V11 | Lab A/B vs status quo on Apple Silicon `macos-release` | Recorded under `benchmarks/results/` | open — two-thread direct slot diagnostic is +7.14% over the equivalent shared-slot protocol, but affinity was unavailable and this is not the official paired runtime |
| V12 | **Hard reject** if worker-affine PUT 2t regresses >5% median vs interleaved baseline | Same as V11 | open — adopt-only throughput is positive but sampled p99 regressed 5.86%; the L1-hot GET mode improves publication p99 9.54% with unchanged GET p99, yet real paired GET/PUT, pinned Linux and correctly labelled macOS rows remain required |
| V13 | ASan/UBSan/TSAN clean on paired + publish stress | CI jobs | **prototype-partial** — `ADR 0036 V13…` + full `paired Store` green under `macos-tsan` (`…/local-macos-2026-08-02-adr0036-tsan/`) and `macos-asan` ASan+UBSan (`…/local-macos-2026-08-02-adr0036-asan/`); full multi-OS CI matrix still open |
| V14 | Crash / fault matrix unchanged for durability boundaries (no ACK-before-publish) | Crash harness subset | **production-baseline** — full sync `crash_persistence --mode matrix` (91 checkpoints) under `macos-release` (`…/local-macos-2026-08-02-adr0036-v14-sync-matrix/`); also crash-daemon pre-commit/post-ack + group-matrix. Slot-pool candidate still open |

Prototype reference (non-production): `src/experimental/paired_shard.cpp` (`publish`,
`reclaim_generations`, `acquire_generation_slot`, `adopt_publication`, `pin_read_generation`,
`stop_and_drain`).

### Prototype protocol hardening — 2026-08-27

The lab prototype no longer release-publishes a pointer whose address can repeat after slot reuse.
It publishes one 64-bit token `{epoch:48, slot+1:16}`. The Writer fully initializes the selected
slot and its descriptor before the release store; the Reader acquire-loads the token, decodes the
slot, and rejects an epoch mismatch before changing its local publication. `visible_through` and
the generation graph remain immutable slot fields made visible by that same release/acquire edge.
The 48-bit epoch limit is checked before mutation publication and fails the batch explicitly on
overflow. A dedicated V1 test forces actual slot reincarnation and checks epoch, visibility,
sequence, and value coherence; the focused ADR suite passes in Release, ASan+UBSan, and TSan.

This strengthens **prototype evidence only**. It does not close V8 in production, V11, V12 or V14,
does not replace the production `shared_ptr`/`ReadLease` path, and does not change this ADR's
proposed status.
The local diagnostic A/B is retained in
`benchmarks/results/adr0036-slot-token-2026-08-27/`; raw paired medians were within -1.8% of the
preceding pointer run and control-normalized ratios were within -1.2%, but the campaign is not a
production-congruent or worker-affine V11/V12 proof.

### V8 production-congruent lifetime candidate — 2026-08-27

`src/experimental/generation_slot_pool.hpp` isolates the slot lifetime protocol while reusing the
real production `PairReadGeneration` graph. It uses the existing production frontier equation
`min(adopted_epoch, minimum_borrowed_epoch)` and reclaims only retired epochs strictly below that
frontier. Regressions of an already published safe frontier fail closed: a late borrow cannot
resurrect an epoch that may already have been reclaimed. Dedicated integration tests construct
snapshots from the durable catalog, retain a cold
read from the old Segment generation through compaction, publish the refreshed graph, complete the
I/O, then prove retirement. A second test covers Writer-owned rotation and the resulting two-record
snapshot. The protocol and proof boundary are documented in
`docs/architecture/generation-slot-pool-candidate.md`.

This moved V8 from “candidate open” to **candidate evidence**, not production closure. At that
stage the candidate still accepted an already heap-built
`shared_ptr<const PairReadGeneration>` per slot and therefore did not demonstrate the
allocation/performance purpose of production slot construction. The later fixed-shell extension
below removes that specific lab blocker; runtime integration and performance proof remain open.

### V6 reservation-before-mutation candidate — 2026-08-27

The candidate now exposes a bounded Writer-only reservation guard. Slot exhaustion is decided
before Store entry. A reservation cancelled before Store linearization is a safe rejection; after
`mark_store_linearized()`, destruction without a successful release publication invokes the
configured fail-closed hook. A durable integration test commits a real PUT, forces generation
commit failure, verifies Store fail-closed, then uses the existing `allow_fail_closed` snapshot
authority to rebuild and publish a generation containing the committed key.

This is **candidate evidence** for V6 ordering and recovery, not production closure. The official
runtime has not adopted reservations and its existing fail-closed/drain machinery is unchanged.

### Fixed shell-storage candidate — 2026-08-27

The candidate now composes each reserved slot with a bounded, preallocated shell storage. A
private construction bridge uses `allocate_shared` to place the real `PairReadGeneration`, its
embedded `DeltaState`, and the shared control block in that storage. The allocator retains the
backing object through final weak destruction; an occupied slot rejects instead of falling back to
the heap. Tests prove address reuse, weak-owner fencing, backing-owner lifetime, and matching
reservation/storage reincarnation with the real generation graph.

This removes the earlier candidate blocker where every slot merely accepted an already heap-built
generation. It still does **not** close V11/V12: COW pages, spines, arena blocks and keys can still
allocate, Writer-side shared ownership remains, and the 512-byte shell ABI budget needs every
supported toolchain row. The bridge is dormant in the core because the production `DeltaState` is
private; its access header is non-installed and neither `ShardPairRuntime` nor `glifistored` can
select it.

The follow-up inline owner removes the backing-storage `shared_ptr` by nesting the bank and pool in
one non-movable lifetime domain. Destruction order and a private builder prevent a generation from
outliving its slot bytes; Writer-only occupancy needs no atomics. It is 7.53% faster than the
otherwise equivalent shared-backing pool in the local diagnostic and passes a 10,000-publication
Writer/Reader TSan stress. However, it remains 7.09% behind plain `make_shared` in the same
construction campaign. Therefore this custom-allocator branch is retained as rejected diagnostic
evidence, not promoted. A viable production candidate must remove the shared control block itself.
External merge state is deliberately unsupported because it could retain the generation beyond the
bank; a future direct-object pool must own merge lifetime in the same destruction domain.

The direct-object follow-up removes `allocate_shared` and the control block entirely. Official and
direct construction share one validation/delta-build routine; the candidate uses `construct_at`
and `destroy_at` over two fixed Writer slots. A 256-publication differential test matches epoch,
visibility, delta state and values, while rejected construction preserves the current generation.
The local median is 3.267.796 publications/s versus 2.791.071 for `make_shared` (**+17.08%**).

The direct-object slot-pool follow-up composes the same construction with bounded reservation,
release/acquire token, Reader safe epoch, cold borrow, Writer reclamation and terminal shutdown.
Epoch zero is direct as well, so the pool has no generation `shared_ptr` or control block. Focused
Release, ASan+UBSan and TSan tests pass, including 10.000 concurrent publications. Its synchronous
protocol median is 3.186.299 publications/s: +7,43% over `make_shared`, with an 8,28% cost versus
the unsafe synchronous ring.

This remains candidate evidence. Reader and Writer are separate threads in the follow-up below,
but physical affinity, merge ownership, durable refresh, lane/socket shutdown and the official
runtime remain untouched. V1–V14 production status is unchanged.

The follow-up diagnostic runs persistent Reader and Writer threads over equivalent 65-slot shared
and direct protocols. Direct construction reaches a 1,275,537 publications/s median versus
1,190,497 shared (**+7.14%**) and lowers aggregate ns/publication by 6.67%. Sampled publication p50
is unchanged; sampled p99 is 5.86% worse. Requested affinity was unavailable on the macOS host, so
the campaign is concurrent but not physically pinned. Raw results and limitations are retained in
`benchmarks/results/adr0036-direct-publication-2026-08-27/`.

Repeated Reader adoption also stops rewriting `reader_safe_epoch` while the frontier is unchanged.
This does not weaken reclamation: no older epoch becomes reclaimable until the frontier advances,
and that advancing store retains release semantics. ASan+UBSan and TSan runs cover the two-thread
runner after this optimization.

With one real `PairReadGeneration::get` per adoption, direct construction reaches 919,357 versus
817,965 publications/s (**+12.40%**), improves sampled publication p50/p99 by 9.09%/9.54%, and
leaves sampled GET p50/p99 unchanged at 83/250 ns. This mode uses a two-byte L1-hot value and still
lacks socket/protocol work and affinity, so it strengthens direction without closing a gate.

### Wave 1 production slot pool (opt-in) — 2026-08-27

`GenerationSlotPool` lands in `src/store/paired/` behind
`PairedConcurrencyConfig::generation_slot_pool` (default **false**). Requirements retained:

- Publication token `{epoch:48, slot+1:16}` via `GenerationPublicationToken`; epoch overflow beyond
  the 48-bit field fails publication with `epoch_exhausted` / arithmetic overflow before a torn
  token can be released.
- Slot reincarnation is allowed only after reclaim of retired epochs strictly below the Reader
  safe frontier **and** `pins == 0`; V1 production unit forces ≥10k reincarnations with token
  epoch/slot coherence.
- Reserve-before-Store-mutate; abandoned linearized reservations trip fail-closed (V6).
- Capacity 65 (`GenerationSlotCapacity<64>`); exhaustion is pre-Store backpressure (V9).
- V7 production litmus covers embedded and dedicated Writer merge/post-cut publication under a
  held Reader frontier. Terminal shutdown discards only unfinished Writer-private merge state after
  Writer join and Reader-lease validation, releasing its merge-cut slot pin before final reclaim.

Default Alternative A remains bit-identical. V11/V12 stay **open**. Durable-group
Writer sites under the flag remain partially dual-pathed; sync/async incremental
plus refresh/merge finish are covered. No production-ready / E3/E4 claim.
sites under the flag are not yet fully dual-pathed. Evidence:
`engineering/evidence/adr0036-production-slot-local-2026-08-27.md` (`local`).

## Compatibility with landed hot-path work

The following remain valid under Alternative A and must keep working under any slot-pool landing:

- `Store::put_batch` sync coalesce (≤32)
- `PublishedAdmission::caller_holds_guard`
- Embedded `DeltaState` in `make_shared` generation allocation
- Proportional reclaim quantum
- Phase scopes (`encode_copy`, `index_publish`, `worker_apply`, `publish`, `ack`)

## Note on mutation-lifecycle structural refactor (2026-08)

The behavior-neutral extraction of `mutation_state` / `fail_closed_state` /
`connection_lifecycle` / `lane_state` nesting does **not** implement this ADR.
Publish remains Writer-owned `shared_ptr` + release-store of a raw generation
pointer. This ADR stays **proposed**.

## References

- [ADR 0031](paired-reader-writer-shards.md) — paired Reader–Writer; publication and reclaim
- [ADR 0032](0032-paired-concurrency-embedded-store.md) — embedded paired concurrency
- [ADR 0035](0035-generation-shell-recycling.md) — rejected shell freelist; deferred full slot pool
- [hot-path-performance-2026-08-01.md](../architecture/hot-path-performance-2026-08-01.md)
- [hot-path-rejected-optimizations.md](../architecture/hot-path-rejected-optimizations.md)
- [paired-shard-volatile-prototype.md](../architecture/paired-shard-volatile-prototype.md)
- Lab: `benchmarks/results/local-macos-2026-08-02-encode-size/`,
  `.../local-macos-2026-08-02-embed-delta/`, `.../local-macos-2026-08-02-gen-shell/`
