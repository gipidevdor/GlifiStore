# ADR 0049: Govern read capacity with bounded adaptive helpers

- Status: proposed
- Date: 2026-11-21
- Deciders: pending design review; plan requested by the maintainer, prepared by Codex
- Applies to: proposed volatile TCP GET helpers and capacity control
- Would amend: [0031](paired-reader-writer-shards.md), [0032](0032-paired-concurrency-embedded-store.md)
  and [0048](0048-volatile-tcp-response-borrow.md), only after acceptance and proofs
- Supersedes: none

## Context

HAZ-045 exposes fixed-owner read capacity while other CPUs may be available. The maintainer
requests measured read capacity, use of all useful available capacity and an initial 70%
hot-owner expansion example, subsequently allowing better thresholds and emphasizing protection
of hardware and integration investments. Existing finite-loop evidence and ADR 0048 do not establish a
sustainable capacity curve or a multi-reader reclamation protocol.

## Proposed decision

Follow the [read management plan](../architecture/adaptive-read-management.md). Keep one
authoritative Writer and one socket-owning Reactor per shard. A shared bounded helper pool
may execute read batches against that same owner's immutable generations. Parked helpers
are offline readers; waking, draining and reassignment retain explicit resource ownership.

Use local observers and one global resource arbiter. Separate the hard helper/memory limits
from a dynamic CPU target. Expand on sustained qualified read pressure; the revised experiment
starts with 65/45% candidates and qualifies thresholds by latency growth, noise and actuation delay;
retire for quiet only after checking capacity with one fewer helper. Allocate fairly between
hot owners and lend all unused eligible capacity to owners with demand. Do not infer spare
CPU from configured thread counts or assume linear scaling with helpers.

Before implementation, specify and prove protection across all daemon readers, including
registration/adoption races and leases crossing completion or large-value continuation paths.
The current daemon epoch frontier does not automatically include embedded read-lease counters.
Keep response order, post-mutation visibility barriers, bounded result credits and safe drain.
Helper pressure alone must not introduce client rejection, automatic reroute or mutation retry.

Thresholds remain fixed within a qualified profile revision. The plan compares candidate
settings, includes wakeup and observation lead time, and widens retirement hysteresis when
needed. It does not claim that 65/45% is optimal. Qualification also measures useful work per
resource and compares primary, fixed-helper and adaptive paths; no economic return is asserted.

## Alternatives and boundaries

Always busy-spinning or forcing 100% CPU without useful demand wastes resources. One private
pool per shard can oversubscribe the process. A 70% threshold without a capacity definition,
global budget or counterfactual scale-down can oscillate. Blindly importing request shedding
from adaptive proxy controllers would change client outcomes. These are not the proposed model.

Online ownership changes, a second Writer, cold-I/O scaling and durable helper reads remain
separate work. The proposed change shares execution of reads; it does not migrate keys or
amend ADR 0033's deferred online reshard contract.

## Status and verification

The [mathematical analysis](../architecture/adaptive-read-model.md) gives conditional arguments
for credit invariants and local hysteresis. Global queue stability, starvation freedom,
latency bounds and throughput optimality are not established. Capacity depends on other
owners' allocations and shared resources; a 70% trigger alone proves none of these properties.

This record is not authoritative until accepted. No helper, controller or capacity metric
described here is implemented. Requirements remain draft and `GATE-ADAPTIVE-READERS` stays
at design stage. The plan fixes milestones and the future safety/performance matrix; metadata
checks or this document's presence cannot close HAZ-045, HAZ-046 or a release gate.
