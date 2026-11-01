# ADR 0012: Worker-affine Reactor execution

- Status: amended by ADR 0016
- Date: 2026-07-19
- Owners: networking and storage maintainers
- Related: ADR 0005, ADR 0006, ADR 0013
- Amended by: ADR 0016 for durable cold reads

## Context

One general Reactor that forwards every request to a Store Worker adds a queue, wakeup, completion,
and cache-line transfer to the steady-state path. Searching other Workers after a miss would also
break deterministic ownership and make scaling dependent on cross-Worker traffic.

## Decision drivers

- preserve one authoritative owner for each key;
- keep steady-state network and Store execution on one executor;
- bound cross-thread queues and memory;
- support epoll and kqueue without platform-specific storage semantics.

## Alternatives considered

- one Reactor plus a shared Worker pool: rejected because every request crosses an executor boundary;
- independent Reactors with remote per-request forwarding: rejected because uniform traffic becomes a queue mesh;
- search peer Workers on miss: rejected because it weakens ownership and makes misses scale linearly.

## Decision

Run one Reactor/executor per Store Worker. A new connection starts on an executor, initializes, and
binds exactly once to a Worker. When necessary, transfer the connection object once through a
bounded MPSC queue; the destination becomes its sole owner. Thereafter it accepts only keys owned by
that Worker. Misdirected requests receive `wrong_owner` and routing metadata; they are not forwarded.

## Consequences

Steady-state requests avoid cross-executor queues and retain Worker cache affinity. Clients need a
connection strategy covering the Workers they use. Handoff saturation restores the connection on the producer, returns wire `OVERLOADED` for that
`BIND_WORKER` when flush is still possible, then closes rather than allocating an unbounded backlog. Online Worker-count changes require an explicit epoch and
rebalance protocol.

## Compatibility and verification

This decision defines server architecture, not persistent bytes. Protocol v2 makes binding and
wrong-owner behavior externally observable. Integration tests must prove one-time ownership
transfer, stale event-token rejection, queue saturation, partial frames, shutdown, and no source
access after publication to the destination.

## Amendment — 2026-11-01

[ADR 0041](0041-bounded-network-security-lifecycles.md) amends network-security lifetimes,
output drain, request-attempt admission and audit emission. It preserves persistence v1,
wire v2 and Store acknowledgement points; its explicit disconnect and quota rules govern
stalled or abusive peers.
