Status: descriptive
Applies to: repository version `0.1.x` (Phase B1)
Owner: maintainers
Last reviewed: 2026-11-07

# Atomics and memory-order inventory

Inventory of shared atomics that participate in correctness. Normative rules:
[`docs/spec/concurrency-memory-model.md`](../spec/concurrency-memory-model.md).
Requirement: `GS-CONCUR-MEM-001`.

The machine-checked companion
[`engineering/evidence/memory-order-inventory-2026-08-30.json`](../../engineering/evidence/memory-order-inventory-2026-08-30.json)
records all 1,215 explicit `std::memory_order_*` occurrences in production headers/sources with exact
file, line, object expression, operation, writer/readers, protected data, required happens-before,
invariant, and positive `relaxed` justification. It is regenerated and checked by
`engineering/tools/audit_memory_orders.py`; a new source occurrence, an unsupported order, an ambiguous
domain, or stale evidence fails closed. This is coverage of the audit inventory, not a formal proof that
each argument is correct.

The current inventory covers 18 domains, including bounded compaction-journal overflow and owning
blob-read materialization admission. Journal entries require the separate commit-gate ownership
handoff; the overflow flag alone does not publish them. The blob byte counter reserves capacity
without publishing another read's payload. These arguments are recorded in the
[concurrency memory model](../spec/concurrency-memory-model.md#8-atomics-and-memory-ordering).

## 1. Paired ReadGeneration publication

| Variable | W / R | Orders | HB / role | Proof |
| --- | --- | --- | --- | --- |
| `Lane::published_generation` | Writer / Readers | release / acquire (Alt A); **relaxed** when dual-published with token | Publish ⇒ observe generation | linearizability tests, TLA+ |
| `Lane::published_token` | Writer / Readers | release / acquire | ADR 0036 opt-in `{epoch,slot}` token; dual-published with pointer mirror while Wave 1 lands. Pointer mirror is **relaxed**; token release is the sole publish edge. Writer ACK-after-visibility and Reader adopt both use `load_published_generation` (DualPath) | generation_slot_pool_production_tests; paired durable-sync ACK litmus |
| `Lane::published_catalog_revision` | Writer / Readers | release / acquire | Durable refresh coherence | durable paired paths |
| `Lane::reader_safe_epoch` | Reader / Writer reclaim | acq_rel CAS (skip when unchanged) | Min leased epoch before free. Wave 2: Readers load-relaxed then CAS only when the frontier moves — avoids redundant release RMW on steady-state GET/adopt | generation lease tests; adopt path |
| `Lane::active_read_leases` | Readers / Writer | relaxed + seq_cst fence before adopt | Closes UAF window | ASan CI |
| `Lane::reclaim_requested` | Readers / Writer | release / acq_rel | Opportunistic reclaim flag; last-lease drop may set without waking Writer (embedded GET) | paired GET litmus |
| `healthy_` | Writer fail-closed / observers | release / acquire | Terminal unavailability | fail-closed paths |

## 2. Admission and shutdown

| Variable | W / R | Orders | Role | Proof |
| --- | --- | --- | --- | --- |
| `admission_state_` | submitters / drain | acq_rel | Closed bit + count | close / LIVE-001 |
| `stopping_` / `Lane::stopping` | closer / Writer | acq_rel / release+acquire | Stop then wake | close tests |
| `active_writers_` | Writers / closer | acq_rel / acquire | Drain observes exit | stop_and_drain |
| `SyncMutation::done` | Writer / caller | release / acquire | Mutation completion; waiters use pause→yield→park | put/erase tests |
| `Lane::signal` | submitters / Writer | release fetch_add + notify / acquire wait | Writer idle wakeup ladder (pause→yield→park) | combining + LIVE-001 |
| Store `active_operations[]` | ops / close | relaxed admit; acq_rel finish | Close LP + drain | GS-CORE-CLOSE-001 |
| Store `lifecycle` | closer / waiters | acq_rel / release / acquire | One-way close | close tests |

## 3. Queues

| Variable | Orders | Role | Proof |
| --- | --- | --- | --- |
| SPSC `head`/`tail` | release publish; acquire consume | Cell ownership; full → backpressure (no overwrite) | unit tests + TSan; Wave 2 hot-producer / slow-consumer litmus |
| MPSC cell `sequence` | release / acquire | Connection handoff | bounded_mpsc tests |

Both queue families keep capacity below half the unsigned cursor range. MPSC ordering uses unsigned modular
distance (never signed subtraction), and both constructors reject a capacity for which wrap ordering would be
ambiguous. Boundary tests exercise the relation directly around `SIZE_MAX`.

## 4. Coherent process configuration

| Variable | W / R | Orders | Role | Proof |
| --- | --- | --- | --- | --- |
| Worker routing revision + algorithm/seed | serialized writers / readers | seq_cst payload/revision; acquire/release writer flag | Readers return one complete, non-wrapping configuration revision; no torn algorithm/seed pair | concurrent 100,000-update regression |
| Index seed | daemon startup / Index constructors | relaxed | One self-contained scalar, configured before worker publication; no associated payload | index seed tests |

## 5. Telemetry-only (relaxed by policy)

Latency histograms, high-water marks, admit/reject counters, merge stats, `LaneMetrics`
(Writer-updated; starts on its own `alignas(128)` line so dense RMWs do not false-share with
`active_read_leases`). Must not publish object contents; promoting a counter to a correctness
edge requires updating this inventory and the concurrency spec.

## 6. GET allocation contract (Wave 2)

Paired volatile `Store::get` / `get_copy` for values ≤ `OwnedBytes::kInlineBytes` (64):

- `ReadLease` adopts a raw generation pointer (no `shared_ptr` control block on the hot path).
- Value materialization uses SSO; `OwnedValue::from_bytes` does not heap for ≤64 B.
- Proof: `glifistore_allocation_fault_tests` `run_paired_volatile_get_inline_zero_heap`
  (evidence class **local** / CI when the suite runs). Durable cold GET and >64 B copies may allocate.

## 7. Reuse and residual gaps

- The line-level inventory mechanically classifies every explicit order, including every `relaxed` occurrence;
  the semantic justifications remain manually reviewed arguments rather than model-checked C++ proofs.
- Multi-shard linearizability relies on routing independence.
- Durable Manifest publication uses mutex/CV domains (see concurrency spec).
- durable_sync combiner still publishes per committed mutation (multi-op generation coalesce deferred; RAW polarity unchanged).
- ADR 0036 default remains Alternative A until V11/V12 + ADR accept.
- Counter exhaustion and reuse reasoning is detailed in the
  [ABA/generation/token audit](aba-generation-token-audit.md); exhaustive operational wrap campaigns remain
  infeasible on this host.
