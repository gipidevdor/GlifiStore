# ADR 0049: Manage read capacity with bounded adaptive helpers

- Status: accepted for the opt-in prototype described below
- Date: 2026-11-21
- Deciders: maintainer instruction “Implementa corri”, implemented by Codex; no release acceptance
- Applies to: experimental volatile TCP GET helpers and capacity control
- Amends: [0031](paired-reader-writer-shards.md), [0032](0032-paired-concurrency-embedded-store.md)
  and [0048](0048-volatile-tcp-response-borrow.md), for the explicit opt-in prototype
- Supersedes: none

## Context

HAZ-045 exposes fixed-owner read capacity while other CPUs may be available. The maintainer
requests measured read capacity, use of all useful available capacity and an initial 70%
hot-owner expansion example, subsequently allowing better thresholds and emphasizing protection
of hardware and integration investments. Existing finite-loop evidence and ADR 0048 do not establish a
sustainable capacity curve or a multi-reader reclamation protocol.

## Decision

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

The implementation must protect all daemon readers, including
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

The implementation is opt-in and remains an architectural prototype. The default helper cap
is zero. No shipped capacity profile is qualified; without one, the adaptive path parks every
helper. `--read-helper-cap N --read-helper-fixed-owner S` explicitly selects a fixed experiment,
not a calibrated adaptive policy. C++ `ReactorConfig::read_helpers.profiles` accepts externally
qualified, revisioned capacity curves with monotonic-clock expiry. Durable helper configuration
is rejected. No on-disk format, wire v2 field, Writer authority, ACK or retry contract changes.

## Exact-generation handoff and bounded continuations

The prototype delegates at most one volatile GET per connection, and at most one task or
unconsumed result per helper (hard process cap 16). The primary checks owner, authorization,
Store admission and the connection mutation-visibility watermark first. It registers the
already-adopted generation's epoch in its existing asynchronous lease table **before**
release-publishing the task. The helper never loads a publication pointer or adopts an epoch.
Every subsequent primary adoption includes all outstanding helper epochs in the minimum.
This extends the single publisher of `reader_safe_epoch`; it does not add an unsafe independent
reader registration or use the embedded lease counter in daemon mode. Both ordinary retired
generations and ADR 0036 slot reclamation use that same minimum.

One cell owns a key of at most 1024 bytes, hash, fixed TTL time, ConnectionToken, request id,
generation pointer and result. Longer keys or unavailable cells use the primary path. The
helper validates the exact immutable Record and returns a view/error code, with no copied value
or second lookup. The origin lease persists through completion acquisition, cancellation and
primary encoding. In particular, a large result is a typed exact-selection continuation: the
primary materializes the selected bytes into its ordinary owning scatter output before dropping
the epoch. It never repeats a GET against a newer version. Responses are delegated only when
this connection has no output or cold-read window; later requests wait for completion. Existing
output limits still apply. A full helper pool does not create a client rejection.

The cell's release/acquire ownership cycle is empty -> reserved -> pending -> running -> complete
-> consuming -> empty. Reassignment requires an empty cell and closes admission before draining.
Arming and draining retain their controller credit until acknowledged. Closed/reused connection
tokens discard results after accounting. Server join joins helpers and discards pending views
before Store teardown, also after an executor error or shutdown timeout. Parked/idle helpers
hold no generation and wait on an atomic signal; they do not poll or create threads per GET.

## Deterministic control and current boundaries

The pure controller uses bounded arrays, integer thresholds, caller-supplied logical milliseconds,
non-overlapping contiguous >=50ms windows and stable equal-weight round-robin ties. Two hot
windows request one grant; a grant requires a strictly beneficial next capacity, a valid profile
and a global soft target below the hard cap. A grant settles for 500ms before further votes.
Quiet retirement requires five seconds below the threshold projected onto C(k-1). Profile
changes reset votes; unknown, expired or stale profiles drain. Budget shrink overrides ordinary
lease duration but retains occupied credits through the drain. No timer is a wall-clock deadline
or scheduler guarantee. Weighted CPU-deficit arbitration remains future work.

A periodic adapter estimates available CPU from whole-process CPU minus measured helper CPU,
with half a CPU reserved as headroom. Configured CPU limit/hardware concurrency is an estimate,
not an OS reservation. Sampled primary lookup/validation CPU excludes socket, Writer and cold-I/O
waits; completed throughput alone never supplies capacity. Equal-share fairness and credit/replay
properties have executable tests; global stability and workload-independent fairness do not.

The offered-rate driver uses the canonical Python wire codec, exact scheduled times independent
of responses, bounded outstanding admissions, explicit missed offers, response-byte/identity
validation and scheduled-to-response latency. Results are always unqualified local experiments.
It preserves failed runs and server CPU observations, never converts a finite throughput result
into an adaptive profile. Remaining obligations include sustained multi-owner/mixed workloads,
large-value resource costs, profile drift, coupled capacities, enable/disable controls and native
platform qualification. See the retained local evidence under
`engineering/evidence/read-capacity-local-2026-11-21/README.md`. HAZ-045/046/047 remain open.
