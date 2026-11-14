# Mutation lifecycle

**Descriptive** · `ShardPairRuntime::run` and the extracted Writer TUs · reviewed 2026-08-30

Requirement: `GS-CONCUR-PAIR-001`

This document maps the **current** paired Writer mutation lifecycle in `ShardPairRuntime::run` and
the Writer TUs `writer_loop`, `writer_sync`, `writer_async` and `sync_lane`, as the baseline for
behavior-neutral refactoring. It does not authorize protocol, ACK polarity, or fail-closed changes.

Structural refactoring must preserve every outcome described here. When code and this text diverge,
prefer the stricter fail-closed reading and update this document in the same commit as the
behavioral lock.

Authoritative wire polarity: [error taxonomy v1](error-taxonomy-v1.md),
[client semantics v1](client-semantics-v1.md).

## 1. Three authorities (must stay distinct)

| Authority | Meaning | Today encoded as |
| --- | --- | --- |
| **Persistent** | Mutation is part of catalog / Index / durable commit | `DurableMutationOutcome` on `DurableMutationResult` |
| **Reader visibility** | Mutation is observable via published generation | `generation_published`, drain snapshot, `publish_incremental` + `publish_read_generation` |
| **Client completion** | ACK polarity decided and delivered | `SyncMutation::status` / `MutationOutcome::error` + `done` / `deliver_outcome` |

Illegal conflations to avoid when extracting types:

- treating key presence alone as commit proof;
- rewriting post-commit failures to `resource_exhausted` (OVERLOADED);
- success-ACK before required visibility;
- changing a decided completion after a later sibling error (except documented sticky visibility upgrade).

Writer ACK-after-visibility and sticky upgrades must observe the published generation through
`load_published_generation` (ADR 0036 DualPath: acquire on `{epoch,slot}` token when the opt-in
slot pool is enabled, otherwise acquire on the mirrored pointer). The same loader is used for
Reader adopt; do not reintroduce a Writer-only relaxed pointer load for ACK decisions.

## 2. Typed stages (target model; mirrors existing flags)

```text
CommitKnowledge  ↔  DurableMutationOutcome
  known_not_committed  ↔  not_committed
  committed            ↔  committed
  indeterminate        ↔  indeterminate

PublicationState
  not_required | required | staged | published | failed

MutationStage (orchestration)
  not_admitted → admitted → staged
       ↓              ↓
  expired_pre_store   durable_started
                           ↓
              authority_committed | known_not_committed | indeterminate
                           ↓
              publication_required → published | publication_failed
                           ↓
              completion_decided → completed
```

Source flags in the dedicated Writer path (`shard_pair_runtime_writer_*.cpp` / `run` orchestration):

| Conceptual stage | Representative flags |
| --- | --- |
| Admitted | `begin_submission`, `healthy_`, `started_`, `!stopping_`; token acquire **or** enqueue for combiner (ADR 0037) |
| Staged | sync LIFO→FIFO combine / async SPSC coalesce |
| Expired pre-Store | `expired[]`, `expire_remaining_`, merge/retire pressure |
| Durable started | `durable_mutate_entered`, `mutate_inflight` |
| Authority | `result.committed()`, `durable_committed`, `durable_commit_observed` |
| Publication | `publication_required`, `generation_published`, `sibling_snapshot_published`, `post_commit_publication_failure` |
| Completion locked | `status_resolved` |
| Fail-closed | `publish_fail_closed` → `healthy_=false`, `expire_remaining_` |

`Admitted` does **not** require handoff to a dedicated Writer thread when combining is enabled
([ADR 0037](../adr/0037-shard-execution-token-flat-combining.md)): the caller that holds the
execution token is the Writer for that turn.

The async Writer may combine several already-admitted FIFO mutations into one publication, bounded
by record count and admission bytes. Volatile mode never waits to manufacture a batch and never
changes per-mutation Store order. After completion outcomes are delivered, one wakeup per distinct
Reader target group is sufficient because the Reader drains the completion SPSC queue; the
official single-Reader pair therefore receives one wakeup per batch. The ACK decision itself
remains per request. Admission retains one completion slot plus its byte credit until that
completion is drained: the preallocated payload slot is released by the Reader only after it pops
the outcome. In durable-group mode the oldest FIFO admission deadline caps the collection wait. If
that inclusive deadline is reached before Store entry, the mutation remains known-not-committed and
completes as overload; the Writer does not wait onward to the durability batch deadline. On the
dedicated runtime, synchronous work is split into at most 32-record turns. An older remainder stays
in a Writer-local FIFO continuation, but an already-admitted async batch runs between quanta. This
prevents either repeated sync admissions or one large caller batch from monopolizing the Writer.
Both payload and completion rings share the same bounded lane capacity. Capacity exhaustion is a
known-not-committed overload before Store entry.

A synchronous `SyncMutation` node remains caller-owned and stack-backed. The Writer writes its
status and derives every batch/fail-closed decision before publishing `done` with release ordering.
That publication ends the Writer's borrow: it must not dereference the node again. The caller waits
with acquire ordering and may destroy the node immediately after observing completion.

## 3. Linearization points

| Point | When | Effect |
| --- | --- | --- |
| Store authority | Durable mutate returns `committed` / Index publish on volatile | Persistent authority acquired |
| Reader visibility | `publish_read_generation` (or successful drain snapshot publish) | RAW for subsequent GET on published generation |
| ACK decision | Completion policy after visibility rules | Wire success / OVERLOADED / INTERNAL_ERROR |
| Fail-closed | Sticky arm after indeterminate / post-commit publication failure | No new mutations linearized |

Happy durable single-op (sync):

```text
admit → enqueue → put_durable(committed) → capture → publish_incremental
  → publish_read_generation → status={} → done
```

Sticky durable (committed+error or indeterminate):

```text
put_durable → try_drain_durable_snapshot → publish_fail_closed
  → ack_after_published_visibility? → done
```

Terminal shutdown may discard an unfinished `PairReadMerge` only after mutation admission is
stopped, every dedicated Writer is joined, and the complete lane set has no counted Reader lease.
The merge is Writer-private maintenance state and is not published authority; its committed
post-cut mutations already live in the current published generation. Discarding it releases the
slot-pool merge-cut pin before terminal generation reclaim and does not change ACK or visibility.

## 4. Completion policy (characterization)

Inputs: `DurableDecision` × `PublicationDecision` → `CompletionDecision`.

| Durable knowledge | Publication | Completion | Fail-closed | Notes |
| --- | --- | --- | --- | --- |
| known_not_committed | any | known_not_committed | no | Rewrite to OVERLOADED bucket |
| committed (clean) | published | success | no | |
| committed (clean) | failed / not published | indeterminate | yes | Capture/publish fail after commit |
| committed (+error) | published | success if visibility matches | yes | Sticky then ACK-after-visibility |
| committed (+error) | failed | indeterminate | yes | |
| indeterminate | published or failed | indeterminate | yes | Drain attempted |
| mutate entered, throw before outcome | — | indeterminate | yes if Store entered | Placeholders → unavailable |
| expired pre-Store | not_required | known_not_committed | no | Queue wait / pressure |

Volatile: Store append success is not ACK until `generation_published`. Throw after `store_mutated` → fail-closed; if already published, preserve success.

Strict `durable_group` Writer batch ([ADR 0047](../adr/0047-durable-group-rejected-siblings.md)): a
sibling rejected before the append attempt (`rejected_before_append`: erase of an absent key,
live-key budget, validation or admission limit) completes as
known-not-committed with its own error. The sub-batch continues, and the earlier unflushed siblings
commit with it and are acknowledged only after the final commit-slot sync and read publication. Any
other known-not-committed sibling failed during or after the append attempt: the unflushed prefix is
abandoned fail-closed and only sequences covered by `durable_through` stay successful.

## 5. Illegal transitions (must remain impossible)

| From | To | Why illegal |
| --- | --- | --- |
| not_admitted / unprocessed | success | Never ACK unprocessed batch items |
| published | known_not_committed | Visibility already required commit/publish |
| committed | overloaded (resource_exhausted) | Post-commit must stay reconcile / INTERNAL_ERROR |
| completion_decided | different outcome | `status_resolved` must stick |
| fail_closed | admit new linearizing mutate | `healthy_` gate |

## 6. Extraction map (behavior-neutral)

| Module | Responsibility |
| --- | --- |
| `mutation_state` | Typed stages + legal transitions |
| `mutation_execution` | Wire rewrite + durable single-op retry loop |
| `mutation_batch` | FIFO key-dedup sub-batch + ≤32 publication chunk cap |
| `publication_coordinator` | `publish_read_generation`, `install_writer_generation` |
| `lane_publication` | DualPath `load_published_generation` / publish install helpers |
| `mutation_recovery` | Catch → `plan_sync_durable_exception_*` (sync durable single-op wired) |
| `completion_policy` | `decide_completion` + `status_from_completion` / wire codes |
| `fail_closed_state` | Sticky arm (`pair_only` / `pair_and_store`), expire remaining, lane wake |
| `lane_state` | By-value `AsyncLaneState` / `SyncLaneState` / `GenerationState` / `MergeState` / `ReclamationState` / `LaneMetrics` |
| `connection_lifecycle` | `decide_connection_action`, `DecidedOutput`, Reactor drain predicates |
| `shard_pair_runtime_writer_*` | Dedicated Writer `run` loop, sync drain, async batch (structure split) |

## 7. Related tests (golden lock)

- `tests/integration/paired_store_core_tests.cpp` / `paired_store_litmus_tests.cpp` /
  `paired_store_erase_batch_volatile_litmus_tests.cpp` — RAW, batch siblings (including
  `erase_batch` FIFO), fail-closed, known-not-committed vs sticky
- `tests/integration/paired_store_batch_order_tests.cpp` — multi-owner `put_batch` / `erase_batch`
  keep each owner's items in caller order (repeated keys, PUT/ERASE rounds) on volatile and durable
  Stores; the C ABI counterpart is `tests/abi/c_abi_tests.c`
- `tests/unit/owner_partition_tests.cpp` — the stable owner partition behind those batches equals
  a stable sort (exhaustive for short inputs, seeded campaigns beyond the small-range sort limits)
- `tests/quality/allocation_fault_batch_tests.cpp` — every allocation failure inside a multi-owner
  batch leaves truthful per-item statuses (`resource_exhausted` only for items not committed)
- `tests/integration/durable_group_rejected_sibling_tests.cpp` — a sibling rejected before the append
  attempt keeps the unflushed siblings of its `durable_group` Writer batch (batch API and daemon lane)
- `tests/integration/server_reactor_durable_lifecycle_tests.cpp` / `server_reactor_durable_wire_tests.cpp` — wire OVERLOADED vs INTERNAL_ERROR, shutdown drain
- `tests/quality/allocation_fault_core_tests.cpp` / `allocation_fault_paired_tests.cpp` /
  `allocation_fault_paired_async_tests.cpp` — ACK-after-publish catch paths and sticky litmus
- `tests/unit/mutation_state_tests.cpp` — transition table + completion characterization
- `tests/unit/completion_policy_recovery_tests.cpp` — Status mapping + sync durable exception plans
- `tests/unit/mutation_extraction_tests.cpp` — batch / rewrite / fail-closed views
- `tests/unit/connection_lifecycle_tests.cpp` — drain decide matrix
- `tests/unit/mutation_lifecycle_property_tests.cpp` — OVERLOADED ⇔ not committed; decided completion sticky
