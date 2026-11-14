# Connection drain state machine

**Descriptive** · `Reactor::Connection`, BIND handoff, half-close, close-after-flush, shutdown · reviewed 2026-08-02

Requirement: `GS-PROTO-WIRE-001`

This page maps the **current** Reactor connection drain and close lifecycle, the baseline for
behavior-neutral refactoring.

Typed helpers live in `connection_lifecycle.hpp`; connection flags on `Reactor::Connection` remain
the storage until a later migration. Structural extraction must preserve every drain/ACK rule below.

Narrative companions: the [server model](../architecture/server-model.md) and the
[graceful drain and overload runbook](../operations/graceful-drain-and-overload.md).

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

## 3. Central decisions

`decide_connection_action(ConnectionDrainSnapshot)` in
`include/glifistore/server/connection_lifecycle.hpp` is the one close-when-drained decision.
`read_ready`, `write_ready` and hangup handling call it through
`reactor_detail::connection_action_for(...)`; a timeout that finds decided output pending flushes it
through `write_ready` before closing (§7). These rules hold on every path:

- close iff `!request_in_flight && !has_pending_output &&` (half-close ⇒ also empty residual input);
- soft-close arms `close_after_flush` and clears further frame admission;
- hangup must not hard-close while decided bytes remain.

## 4. Decided output (ACK object)

“Decided” means encode into `output` / `output_lease` succeeded; decided responses have no object of
their own. `DecidedOutput` in `connection_lifecycle.hpp` is the typed view of decided bytes still
owed to the peer: contiguous output plus the remaining lease header and value. Rules that must stay
true:

- half-close drains decided output before teardown;
- `close_after_flush` refuses new frames but does not discard decided bytes;
- decode failure / partial-frame timeout must not discard a prior decided response
  (`close_after_flush` + drain);
- BIND OK must flush before trailing decode can close;
- handoff failure returns OVERLOADED then `close_after_flush` (definitive status preserved);
- stale `(slot, generation)` completions are dropped without rewriting an earlier ACK;
- after a mutation completion, buffered frames may resume while its decided ACK remains in the
  contiguous output buffer; a trailing failure arms `close_after_flush` and drains every response
  decided earlier in wire order. Resume is suppressed if output was already pending when the
  completion arrived, so socket backpressure remains the admission boundary for that connection.

## 5. Illegal combinations

`decide_connection_action` enforces the drain-before-close row; `process_frames` and the error
paths that arm `close_after_flush` enforce the others.

| Combination | Policy |
| --- | --- |
| `close_after_flush` + accept new Store/BIND frames | Refuse |
| `request_in_flight` + start another Store op | Refuse, except that contiguous PUT and ERASE frames may form a window of up to 32 mutations in flight ([ADR 0037](../adr/0037-shard-execution-token-flat-combining.md)) |
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
