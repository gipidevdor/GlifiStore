# ADR 0047: Rejected siblings in a strict durable group

- Status: accepted (on maintainer direction, 2026-11-14)
- Date: 2026-11-14
- Deciders: storage and concurrency maintainers
- Applies to: `durable_group` Writer batches (embedded Store and `glifistored`); persistence v1 and wire v2
- Amends: none
- Supersedes: none

## Context

A strict `durable_group` Writer takes the queued mutations of one Worker, splits them into
sub-batches of distinct keys in FIFO order, appends each sibling into one pending group, and commits
the group with a single final commit-slot sync. No sibling is acknowledged before that sync and the
read publication that follows it ([ADR 0011](0011-durable-group-commit.md),
[ADR 0037](0037-shard-execution-token-flat-combining.md)).

`StoreAccess::mutate_durable_batch` abandons the unflushed prefix of a sub-batch, and fails the
durable runtime closed, whenever a later sibling returns known-not-committed. The rule protects a real
case: a failure during or after the append attempt can leave the Worker's pending group and the
Segment pending commit disagreeing, and then earlier siblings must not be acknowledged
(`paired durable sync Writer does not success-ACK abandoned unflushed batch siblings`).

The rule is applied to every known-not-committed result, including rejections decided before the
append attempt, which touch neither structure. A rejection of that kind is routine client behavior:

- an erase of an absent key (`not_found`);
- a new key beyond the live-key budget (`resource_exhausted`);
- a validation or admission limit (record size, Index capacity, hot-cache staging, sequence space).

Reproduced before this ADR:

- `erase_batch([A, ABSENT, B])` after puts of `A` and `B`: `A` returns `unavailable` ("orphaned pending
  Segment commit"), `ABSENT` returns `not_found`, `B` returns `resource_exhausted` ("stopped after a
  sticky mutation failure"), and every later mutation returns `unavailable`.
- Daemon, `durable_group`, one connection pipelining `PUT k` and `ERASE absent`: the PUT answers
  `INTERNAL_ERROR`, `GET k` answers `NOT_FOUND`, and later PUTs answer `OVERLOADED` until the daemon
  restarts.

One routine request therefore takes the shard down for every client (HAZ-044).

## Decision drivers

- A request that one client can send must not fail the Store for others.
- Acknowledgement stays after the final commit-slot sync and read publication.
- A failure that may leave the pending group inconsistent stays fail-closed.
- Persistence v1, wire v2 and the C ABI are unchanged; the change is small enough to review.

## Alternatives considered

- **Keep the rule and document it.** Leaves an availability loss that any client can trigger.
- **Exempt `not_found` by status code in the Writer batch.** The smallest diff, but the safety argument
  then rests on a status code, and the other rejections stay fatal.
- **Compare the pending-group size after each failure.** An equal count does not show that the Segment
  pending commit is undisturbed.
- **Flush the prefix before every sibling.** Removes the group commit that `durable_group` exists for.

## Decision

### 1. Rejected before append

A known-not-committed mutation is *rejected before append* when it returns before the runtime takes the
Worker's writable Segment handle. The Worker's pending group and Segment pending commit are then
unchanged, and no sequence number was consumed. The class contains an erase of an absent key, the
live-key budget, Index capacity admission, group publication capacity, record size and encoding
limits, hot-cache staging admission and sequence-space exhaustion. Every other known-not-committed
result (append, rotation, flush, exception paths, compaction Index-gate conflicts) is outside the
class.

### 2. Writer batch rule

A sibling rejected before append reports its own known-not-committed error; codes and wire mapping are
unchanged. The sub-batch continues with the next sibling. The earlier siblings commit with the
sub-batch under the unchanged group commit and are acknowledged only after the final commit-slot sync
and read publication.

Any other known-not-committed sibling keeps the current rule: the unflushed prefix is abandoned
fail-closed, and only sequences already covered by `durable_through` stay successful.

### 3. Conservative default

A result belongs to the class only when its return site says so (`DurableMutationResult::rejected_before_append`,
set by `mutation_rejected`). A new return site defaults to fail-closed. The returns of
`DurableRuntimeCatalog::mutate` that precede the append loop must not change pending state before they
return.

### Unchanged

Acknowledgement points, persistent write ordering, Manifest authority, recovery, sub-batch splitting,
the retry of Index-gate conflicts, and every status code a client can observe for a rejected request.

## Consequences

- A rejected request no longer fails the Store, and its successful siblings are acknowledged normally.
- `DurableMutationResult` gains one field, and the pre-append block of `mutate` gains a contract that
  maintainers must keep: no state change before those returns. A violation would let an inconsistent
  prefix commit. The comment at the block, the default in §3 and the litmus tests guard it.
- Append, rotation and flush failures behind an unflushed prefix still fail the runtime closed.
- Deferred: the remaining known-not-committed outcomes. A compaction Index-gate conflict also returns
  before the append, but no litmus arms the gate behind an unflushed sibling, and its interaction with
  the Writer's final commit needs its own proof. Outcomes at or after the append attempt (for example a
  before-hook failure) need a proof that the Segment pending commit is intact.

## Compatibility and migration

No change to persisted bytes, wire frames, configuration or the C ABI. The observable difference is
limited to requests that previously failed the Store closed: the rejected request keeps its status, its
siblings succeed, and later requests are served. A Store that failed closed under the old rule recovers
as before.

## Verification

- `tests/integration/durable_group_rejected_sibling_tests.cpp`: absent-key erase and live-key budget
  behind an unflushed sibling on the batch API, and a pipelined PUT followed by an erase of an absent
  key through the daemon's asynchronous lane. The first three tests failed before the change and pass
  after it; the Store stays healthy and recovery returns the same state. Two white-box tests call
  `mutate_durable_batch` and assert the mark on both sides of the class: rejections before append are
  marked and keep the prefix, a failure at append is not marked and abandons it. The other returns of the
  class share the structural property of §3 and have no dedicated litmus.
- `tests/integration/paired_store_batch_order_tests.cpp`: erase rounds with many absent keys on a
  four-owner `durable_group` Store, checked against an in-order oracle and after reopen.
- `tests/integration/paired_store_litmus_tests.cpp`: failures at append keep abandoning the prefix.
- Gate `GATE-DURABLE-ACK`; hazard HAZ-044; requirement `GS-PERSIST-ACK-001`.

## References

- [ADR 0011](0011-durable-group-commit.md), [ADR 0037](0037-shard-execution-token-flat-combining.md)
- [Mutation lifecycle](../spec/mutation-lifecycle.md), [persistence v1](../spec/persistence-v1.md)
