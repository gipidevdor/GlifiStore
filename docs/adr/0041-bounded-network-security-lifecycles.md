# ADR 0041: Bounded TLS, output, admission and audit lifecycles

- Status: accepted
- Date: 2026-11-01
- Deciders: security and networking maintainers
- Applies to: paired Reader daemon, TLS outer transport, wire protocol v2
- Amends: ADR 0012, ADR 0019, ADR 0020, ADR 0022
- Supersedes: none

## Context

An incomplete TLS handshake previously waited on a Reader before consuming a bounded connection
slot. Decided output could retain sockets and value leases indefinitely because both ordinary
timeout classes excluded it. Response-byte charging discarded failed charges, and authorization
denials bypassed request-attempt admission before emitting synchronous audit events.

## Decision drivers

- Bound unauthenticated and authenticated peer-controlled work and resource lifetimes.
- Preserve mTLS verification, capability/prefix enforcement and platform backends.
- Preserve ordered decided responses and honest mutation outcomes on disconnect.
- Retain exact audit decision counters while bounding emitted event volume.

## Alternatives considered

- Shorter blocking handshake deadlines still stall unrelated sessions: rejected.
- A new handshake thread pool adds a second queue and shutdown lifecycle: use existing slots/poller.
- Unbounded response drain preserves an impossible delivery promise to a non-reading peer: rejected.
- Discarding oversized output or returning mutation OVERLOADED after commit corrupts outcomes:
  preserve decided output and limit subsequent admissions instead.
- A per-Server audit thread joined on destruction can itself prevent bounded shutdown: rejected.
  Detached per-instance workers accumulate unbounded resources across Server restarts: rejected.
  Never set O_NONBLOCK on a duplicate of stderr: it changes the shared open-file description.

## Decision

1. An accepted TLS socket consumes the same bounded connection slot as an established session.
   The Reader advances SSL_accept once per readiness event, arming WANT_READ/WANT_WRITE interests.
   The original handshake deadline applies even when all abuse settings are disabled; zero selects
   10 seconds. No framing or grants precede successful verification and principal extraction.
   Completion immediately drains potentially coalesced application records on edge-triggered
   kqueue/epoll. Pending handshakes close on stop, error or expiry and release the slot.
   TCP accept batches use the event-batch bound and explicit continuation between Reader turns.
2. Every empty-to-pending output transition starts an output-progress budget. Only positive
   send/SSL_write/sendmsg progress renews it. New input, additional queued output and EAGAIN do not.
   Use request-timeout-ms, or idle-timeout-ms if the former is zero; both zero deliberately disable
   output expiry for trusted deployments. Carry the original timestamp through BIND handoff.
   Expiry stops input, offers a bounded best-effort flush and hard-closes even close_after_flush.
   Count it under abuse_request_timeout_closed. Admitted mutations continue; stale completions
   are discarded. Never manufacture a rejected/not-committed acknowledgement on this path.
3. Charge successfully queued response values irrevocably. A response above the remaining byte
   allowance saturates that principal's one-second window. Later admissions, including zero-byte
   INIT, are refused until reset. This permits one already-decided response to exceed the residual;
   it is an admission bound, not exact packet-level egress shaping.
4. Count complete denied requests in connection/principal attempt and request-byte budgets before
   audit or response side effects. Only authorized HEALTH/READY/STATS retain exemption. Within
   budget, denials remain PERMISSION_DENIED. Exhaustion of a denied-request stream queues
   OVERLOADED after previous decided bytes, stops further input, then closes after bounded drain.
   Ordinary authorized overload retains its existing response/connection contract.
5. One process-wide emitter owns a CLOEXEC duplicate of stderr, captured when the first emitting
   SecurityAudit is constructed (before daemon sandboxing). Its fixed queue holds 128 events,
   each at most 8 KiB, plus one in-flight event. Producers use try_lock and never write to the
   descriptor. Admission and write attempts each have a 128-event monotonic one-second budget.
   The worker blocks SIGPIPE only in its own thread and handles short writes and EINTR. With an
   inherited nonblocking sink, EAGAIN waits for writability in that worker and retains the current
   line/suffix. A hard I/O error disables the sink and suppresses queued/future events, avoiding
   appending a new event to a possibly truncated record. No sink retry/rotation promise is added.
   The worker retains its own sink and shared event counters, never a Server or Audit pointer.
   It lives until process exit and is never joined by Server/Audit destruction; repeated Server
   construction does not allocate more workers or descriptors. O_NONBLOCK and the process signal
   disposition are unchanged. On Darwin, F_SETNOSIGPIPE suppresses process-directed EPIPE signals;
   this attribute is shared with the original stderr and its duplicates. Failure to initialize
   the sink suppresses eligible events and is observable in the suppression counter.
   Exact auth/authz/TLS counters count all decisions. audit_events_suppressed counts contention,
   queue/rate limits, allocation/init and write failures; audit_events_write_failed identifies
   write failures, audit_events_written complete writes, and audit_events_pending outstanding
   work. These are concurrent snapshots, not an atomic tuple. Quiet suppresses accepts only.
   Fields remain escaped, capped and free of payload/key material.

## Consequences

Peer stalls no longer monopolize TLS admission or indefinitely retain decided output. Denial
floods are admitted and logged within explicit budgets. Suppression is observable rather than
being a promise of one persistent event per decision.

Stderr may still block the single emitter indefinitely. It cannot retain a Reader or Server
destructor; the fixed queue fills and subsequent events are suppressed. Operators must drain the
sink to obtain events. Pending events are best effort and may be lost at process exit; no flush,
durable audit, sink rotation/reinitialization, or safe post-initialization fork contract is added.
One worker/descriptor and bounded queued state remain until process exit. Packet-level
per-principal shaping remains residual work.

## Compatibility and migration

Persistence v1, wire v2 bytes/status codes, Store acknowledgement points, routing, visibility and
mutation cancellation rules do not change. A peer that previously retained output beyond all
deadlines can now observe a disconnect. Mutations with sent bytes and no definitive response
remain indeterminate. A denied stream over quota sees OVERLOADED followed by closure rather than
unlimited PERMISSION_DENIED replies. Existing deployments with both timeout knobs zero retain
their trusted, unbounded-drain policy. No production or hostile-public readiness is inferred.

## Verification

- tests/unit/tls_tests.cpp: live TLS sessions alongside stalled handshakes, slot/deadline/stop,
  coalesced Finished+INIT, missing/revoked certificates and existing TLS paths.
- tests/integration/server_security_bounds_tests.cpp: denied mutation/probe attempts, healthy
  probes, non-reading contiguous/scatter responses and slot reuse.
- tests/unit/abuse_limits_tests.cpp: oversized/exact/residual charges, zero-byte follow-ups,
  principal separation, reset and UINT64_MAX configuration.
- tests/unit/security_audit_tests.cpp: exact decision counters, quiet and disabled controls.
- tests/security_audit_sink_tests.cpp: isolated-process JSON/escaping/quiet/storm controls, full
  blocking pipe with healthy single-Reader probe and bounded Server destruction, unchanged
  descriptor flags, closed pipe with default SIGPIPE, and nonblocking backpressure with expanded
  fields larger than portable PIPE_BUF; each has a 15-second CTest deadline.
- Existing drain, handoff, timeout, mutation and client-semantics tests remain required.
- Local evidence: [security bounds campaign](../../engineering/evidence/security-bounds-local-2026-11-01.md).
  Linux epoll and FreeBSD/OpenBSD/LibreSSL execution require their CI rows; local macOS is kqueue.

Darwin behavior was checked against Apple's
[write implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_generic.c)
and [F_SETNOSIGPIPE implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_descrip.c),
then reproduced with the isolated closed-pipe test and default SIGPIPE disposition.

## References

- [Secure profile](../security/secure-profile.md)
- [Connection drain state machine](../spec/connection-drain-state-machine.md)
- [Client semantics](../spec/client-semantics-v1.md)
- GS-SEC-BOUNDS-001; GS-SEC-PROFILE-001; HAZ-024; GATE-AUTH-TRANSPORT
