Status: descriptive of as-implemented Reactor connection drain / close lifecycle (behavior-neutral refactor baseline)
Applies to: `Reactor::Connection`, BIND handoff, half-close, close-after-flush, shutdown
Owner: networking maintainers
Last reviewed: 2026-08-02
Requirement: `GS-PROTO-WIRE-001`

# Connection drain state machine (as implemented)

Maps the **current** Reactor connection lifecycle. Typed helpers live in
`connection_lifecycle.hpp`; connection flags on `Reactor::Connection` remain the
storage until a later migration. Structural extraction must preserve every drain/ACK
rule below.

Narrative companion: [docs/architecture/server-model.md](../architecture/server-model.md),
[docs/operations/graceful-drain-and-overload.md](../operations/graceful-drain-and-overload.md).

## 1. Implicit flags → target lifecycle

| Target `ConnectionLifecycle` | Encoded today by |
| --- | --- |
| `open` | `socket.valid()`, `!peer_read_closed`, `!close_after_flush` |
| `peer_half_closed` | `peer_read_closed` |
| `draining_decided_output` | pending `output` / `output_lease` and/or `request_in_flight` after soft/half close |
| `close_after_flush` | `close_after_flush` (refuse new Store/BIND work; drain then close) |
| `forced_close` | `close_all_connections` / hard request-timeout path (API, not a flag) |
| `closed` | slot freed, `generation++` |

| Target `InputLifecycle` | Encoded today by |
| --- | --- |
| `accepting` | `process_frames` allowed |
| `partial_frame` | residual `input` + `partial_request_since` |
| `stopped` | `close_after_flush` or shutdown probe-only |
| `eof` | `peer_read_closed` |

## 2. State diagram

```text
open
  ├─(peer EOF / TLS closed / hangup)─► peer_half_closed ─► draining_decided_output ─► closed
  ├─(soft reject / partial timeout / decode fail with pending output)─► close_after_flush
  │                                              └─► draining_decided_output ─► closed
  ├─(idle clean)─► closed
  └─(force close / hard timeout)─► forced_close ─► closed (best-effort flush)
```

## 3. Central decisions (today duplicated)

Close-when-drained predicates appear in `read_ready`, `write_ready`, hangup handling, and timeouts:

- close iff `!request_in_flight && !has_pending_output &&` (half-close ⇒ also empty residual input);
- soft-close arms `close_after_flush` and clears further frame admission;
- hangup must not hard-close while decided bytes remain.

Target: one `decide_connection_action(snapshot)` used by all I/O paths.

**Implemented:** `include/glifistore/server/connection_lifecycle.hpp` + Reactor
`read_ready` / `write_ready` / hangup use `connection_action_for(...)` (same predicates).

## 4. Decided output (ACK object)

There is no `DecidedResponse` type yet. “Decided” means encode into `output` / `output_lease`
succeeded. Rules that must stay true:

- half-close drains decided output before teardown;
- `close_after_flush` refuses new frames but does not discard decided bytes;
- decode failure / partial-frame timeout must not discard a prior decided response
  (`close_after_flush` + drain);
- BIND OK must flush before trailing decode can close;
- handoff failure returns OVERLOADED then `close_after_flush` (definitive status preserved);
- stale `(slot, generation)` completions are dropped without rewriting an earlier ACK.
- after a mutation completion, buffered frames may resume while its decided ACK remains in the
  contiguous output buffer; a trailing failure arms `close_after_flush` and drains every response
  decided earlier in wire order. Resume is suppressed if output was already pending when the
  completion arrived, so socket backpressure remains the admission boundary for that connection.

## 5. Illegal combinations (enforced only by scattered ifs today)

| Combination | Policy |
| --- | --- |
| `close_after_flush` + accept new Store/BIND frames | Refuse |
| `request_in_flight` + start another Store op | Refuse |
| `output_lease` + `process_frames` | Wait |
| Close on half-close while output/in-flight/residual input | Drain first |
| Discard decided ACK for later frame error | Soft-close + drain |

## 6. Golden lock tests

- `tests/integration/server_reactor_security_tests.cpp` — half-close drain, partial timeout drain, rate-limit OVERLOADED survives trailing decode failure
- `tests/integration/server_reactor_durable_lifecycle_tests.cpp` /
  `server_reactor_durable_wire_tests.cpp` — BIND OK flush before decode failure, handoff
  saturation OVERLOADED, shutdown drain
- `tests/unit/connection_handoff_tests.cpp` — mesh bound + `stop_accepting`

## 7. Bounded output amendment (ADR 0041)

[ADR 0041](../adr/0041-bounded-network-security-lifecycles.md) bounds the drain rules above.
A decided response is retained only while output makes write progress within request-timeout-ms
(or idle-timeout-ms when request timeout is zero). Both zero disables this bound intentionally.
Queueing starts the budget; only positive socket/TLS/scatter writes renew it. Input and BIND
handoff do not renew it. Expiry offers a bounded best-effort flush and hard-closes, including
close_after_flush and peer-half-closed states. Previously admitted Store mutations continue;
disconnect creates no false OVERLOADED/not-committed acknowledgement.

Pending TLS handshakes occupy bounded slots and use a separate handshake deadline before any
application framing. Over-quota denied streams stop parsing after their ordered OVERLOADED and
close through the same bounded output lifecycle. Authorized lifecycle probes remain exempt.
