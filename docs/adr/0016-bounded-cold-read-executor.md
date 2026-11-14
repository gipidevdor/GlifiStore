# ADR 0016: Bounded durable cold-read executor

- Status: accepted
- Date: 2026-07-19
- Owners: networking and storage maintainers
- Related: ADR 0009, ADR 0012, ADR 0013, ADR 0015

## Context

ADR 0012 keeps ordinary Store execution on the Worker-affine Reactor. Durable cold GET already
performs `pread` and CRC outside Worker and catalog locks by pinning an immutable Segment generation,
but a synchronous call from the TCP path still blocks the complete Reactor and every connection it
owns.

## Decision drivers

- no potentially slow file I/O on a network Reactor;
- preserve exact Worker ownership and protocol-v2 response order;
- make connection, Store, Segment, and buffer lifetimes explicit;
- bound queued work and completion memory;
- use one portable design on epoll and kqueue.

## Alternatives considered

- native `io_uring`/AIO: deferred because support and cancellation semantics differ by platform;
- one disk thread per Reactor: simple but scales thread count with Worker count and serializes all
  cold reads of one Reactor;
- forward complete GETs to a general Worker pool: rejected because hot and volatile reads would pay
  an unnecessary queue hop and lose the ADR 0012 steady-state path;
- an unbounded task queue: rejected because device stalls would become unbounded memory growth.

## Decision

A durable GET is prepared synchronously under short Worker/catalog locks. A hot value or error
returns immediately. A cold preparation owns an independent key, exact `RecordRef`, and
`shared_ptr` generation pin before it leaves the Reactor. The move-only preparation, connection
token, request ID, Worker, cancellation flag, and response byte budget enter a fixed-capacity shared
disk-read executor.

The executor performs record I/O and CRC with no Worker/catalog lock. It posts an owning result to a
fixed-capacity queue belonging to the source Reactor and wakes that Reactor. Completion rechecks that
the Worker index still names the exact reference and generation; on a concurrent mutation it
prepares and executes the current read again. `(slot, generation)` rejects stale completions.

Each connection admits at most one cold read and does not execute later frames until completion.
This preserves strict per-connection ordering without a reorder map. Other connections continue.
Queue saturation returns `OVERLOADED`. Server shutdown stops Reactor admission, cancels queued jobs,
joins in-flight jobs, and only then closes Store or destroys Reactors.

## Consequences

Hot durable and all volatile GETs retain their direct path. Cold GETs pay one executor and completion
hop plus an opaque pin allocation. A blocked device consumes a bounded executor thread and admission
slot, not a network loop. Per-connection pipeline parallelism intentionally stops at a cold GET;
parallel cold reads require multiple connections.

## Compatibility and verification

Persistent formats, routing, acknowledgements, and wire bytes do not change. Protocol-v2 response
order remains unchanged. Deterministic TCP tests block positional reads and verify same-Worker
progress, overload, close cancellation, slot-generation reuse, late-completion rejection, and
shutdown drain. ASan/UBSan and TSan remain required gates.
