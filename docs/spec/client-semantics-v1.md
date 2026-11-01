Status: normative for official GlifiStore TCP clients
Applies to: wire protocol v2 clients (C++, Python, Perl, Go, Erlang, Ruby)
Owner: networking maintainers
Last reviewed: 2026-09-15

# Client semantics v1 — errors, retry, and timeouts

This specification freezes the **observable** failure, retry, and deadline behavior that every
official GlifiStore TCP client must implement. It complements
[wire protocol v2](wire-protocol-v2.md) (bytes on the wire) and is decided by
[ADR 0019](../adr/0019-client-error-retry-timeout.md).

The embedded Store API and daemon-internal error codes are out of scope except where they appear as
wire statuses. Protocol v2 has no authentication or protocol-level timeout field. Optional outer
TLS 1.3 (ADR 0020) is a transport concern and does not add wire opcodes; client TLS connect options
follow [secure-profile.md](../security/secure-profile.md).

## 1. Clocks and deadline kinds

| Deadline | Clock | Covers |
| --- | --- | --- |
| Connect timeout | Platform connect semantics | TCP connect to one peer address after the resolver returns. Time spent inside a blocking system resolver is **not** required to count against this budget. |
| Request timeout | **Monotonic** clock (`steady_clock` / `CLOCK_MONOTONIC` / `time.monotonic`) | Encoding admission through the final validated response for one exchange, or for an entire pipeline batch on one Worker connection. |

Civil/`time.time()` clocks must not drive request deadlines. TTL/`expire_at_ns` remains absolute Unix
nanoseconds on the wire ([wire protocol §9](wire-protocol-v2.md)).

Default configuration values in the reference clients are guidance, not protocol constants:
connect ≈ 3 s, request ≈ 5 s, unless the application overrides them.

## 2. Portable error categories

Official SDKs expose a **category** (name may be an enum, exception class, or string key) drawn from
this closed set for client-visible failures:

| Category | Meaning |
| --- | --- |
| `invalid_argument` | Local validation failed, or the server returned `INVALID_REQUEST` / `UNSUPPORTED`. |
| `not_found` | Server returned `NOT_FOUND`. |
| `overloaded` | Server returned `OVERLOADED`, or a local admission/resource limit equivalent was hit before send. |
| `permission_denied` | Server returned `PERMISSION_DENIED` (secure-profile authz). |
| `unavailable` | Client closed, routing metadata changed, bootstrap/reconnect cannot proceed, or `NOT_BOUND` while the session is considered unusable. |
| `transport` | Socket I/O, connect failure after dial, peer EOF, or **request deadline expired** while waiting on the socket. |
| `protocol` | Framing/codec failure, mismatched `request_id`, wrong Worker owner on an `OK`/`error` that should not occur, non-empty mutation `OK` value, or other trust failure in a decoded frame. |
| `internal` | Server returned `INTERNAL_ERROR`, or the client detected an unrecoverable internal inconsistency. |

Language surface may use exceptions (Python), `Result`/`ErrorCode` (C++), or blessed hashes (Perl).
The **category name above is the cross-language contract**, not the C++ `ErrorCode` enumerator set.
C++ may map categories onto existing `ErrorCode` values; Python/Perl may use exception classes.
Applications that need portability should key off category (and mutation outcome), not language-specific
type names.

### 2.1 Structured fields (required vs optional)

When an error is returned to the application, official clients **must** be able to expose:

| Field | Required | Notes |
| --- | --- | --- |
| `category` | yes | One of the names in §2. |
| `message` | yes | Human-readable; not a parse API. |
| `wire_status` | when a response frame was decoded | Exact `ResponseStatus` integer/name from the wire. |
| `mutation_outcome` | for `PUT`/`ERASE` (and pipeline mutation positions) | `committed` / `rejected` / `indeterminate` (standalone) or pipeline `succeeded` / `failed` / `indeterminate`. |
| `bytes_sent` | when a send was attempted and then failed | Count of request bytes written on that attempt; `0` means the request is known unsent. |
| `request_id` | when known | The client-generated id for the exchange. |
| `worker` | when known | Bound Worker for the connection. |
| `routing_epoch` | when known from session or response | |
| `retryability` | yes (may be derived) | See §4. |
| `operation` | recommended | e.g. `get`, `put`, `erase`, `ping`, `backup`, `pipeline`. |

Perl 0.1.x exposes the full §2.1 field set on `GlifiStore::Error` (category, message, wire status,
mutation outcome, bytes sent, request id, Worker, routing epoch, retryability, operation). Richer
fields must not change category/outcome rules when added.

## 3. Wire status → category and mutation outcome

| Wire status | Typical category | Standalone `PUT`/`ERASE` outcome | Notes |
| --- | --- | --- | --- |
| `OK` | (success) | `committed` only if value is empty | Non-empty mutation `OK` value → `protocol` + `indeterminate`; reset connection. |
| `INVALID_REQUEST` | `invalid_argument` | `rejected` | Safe to correct and retry. |
| `UNSUPPORTED` | `invalid_argument` | `rejected` | |
| `INTERNAL_ERROR` | `internal` | `indeterminate` | Server could not complete; mutation may or may not have applied. |
| `NOT_FOUND` | `not_found` | `rejected` for `ERASE` if returned as failure | Reads raise/return not-found; do not treat as transport failure. Pipeline / `execute_batch` ERASE slots use outcome `failed` with the same portable `mutation_outcome=rejected` / `retryability=new_attempt`. |
| `OVERLOADED` | `overloaded` | `rejected` | Known not committed. Wire collapses admission pressure **and** durable capacity exhaustion (including maintenance emergency `storage_exhausted`). Clients must not infer which cause applied. |
| `WRONG_OWNER` | `protocol` | `rejected` | Mark client **unhealthy**; open a new client (v2 has no online rebalance). |
| `NOT_BOUND` | `unavailable` | `rejected` | Mark client **unhealthy** if observed on a supposedly bound session. |
| `PERMISSION_DENIED` | `permission_denied` | `rejected` | Secure-profile capability denial; `retryability=never`. |

Unknown status codes: treat as error, preserve TCP framing sync, category `protocol` or `internal`;
for mutations prefer `indeterminate` if any request bytes were written, else `rejected`.

## 4. Retryability

| Class | Meaning | Examples |
| --- | --- | --- |
| `never` | Retrying the same logical mutation without application reconciliation may duplicate or is pointless | `committed`; local `invalid_argument` before send; **`overloaded`** (admission and capacity causes are indistinguishable on the wire); **`permission_denied`** |
| `same_request` | Automatic client retry of the **same** encoded attempt is allowed under §5 | Read-only transport failure; mutation with `bytes_sent == 0` |
| `new_attempt` | Application may start a **new** request after fixing inputs or backing off | `not_found`; `rejected` with server validation (not `overloaded`) |
| `reconcile_first` | Must not blindly retry a mutation | `indeterminate`; disconnect after `bytes_sent > 0` |

`retryability` reported to applications must be consistent with these classes.
When `mutation_outcome` is already `indeterminate`, official clients must report
`reconcile_first` even if `bytes_sent` was omitted or is still `0` (a post-send
transport failure must not fall through to `same_request`).
Per-slot pipeline / batch errors follow the same rule: an
`indeterminate` mutation slot must expose `reconcile_first` (and a positive
`bytes_sent` when the request frame left the client), not the bare category
defaults (`same_request` / `new_attempt`).

## 5. Automatic client retries

Official clients implement **at most one** automatic retry for:

1. **`GET` and `PING`** after a transport failure (including deadline while receiving), after resetting
   the Worker connection and re-bootstrap (`INIT` + `BIND_WORKER`) when needed.
2. **`PUT` and `ERASE`** only when the failed send wrote **zero** bytes (`bytes_sent == 0`). After the
   single retry, a further zero-byte failure is `rejected`, not `indeterminate`.

Clients **must not** automatically retry:

- pipelines as a whole;
- multi-Worker `execute_batch` as a whole (same rule: no automatic retry of the batch);
- mutations after any positive `bytes_sent`;
- `INTERNAL_ERROR` or other `indeterminate` outcomes;
- unhealthy clients (routing epoch/count change, wrong owner on bound traffic).

`execute_batch` is a client-side grouping helper: one pipeline per Worker, responses restored to
caller order. It is not atomic across Workers. After any Worker group has been admitted, a
group-level failure must stamp that Worker's slots `failed` (or preserve already-classified
pipeline slots) and still return sibling Worker results — it must not abort the whole call with
a single error that discards committed or indeterminate siblings. A per-Worker connect/rebind
failure before that group's send is likewise a group-level failure (`bytes_sent=0` →
`failed`/`rejected` for that Worker's mutation slots, including portable `mutation_outcome`)
and must not discard siblings that can still run.

Reconnect after a transient failure must accept only the **original** `worker_count` and
`routing_epoch`. A mismatch → `unavailable`, client unhealthy, no further automatic retry.

After transport loss on a Worker connection, the client **must** discard the old socket state and
re-bootstrap that connection with `INIT` followed by exactly one `BIND_WORKER` before any further
Store traffic ([wire protocol §10.1](wire-protocol-v2.md)). Session initialization on a new TCP
stream is mandatory even when routing metadata is unchanged.

Clients **must not** assume server-side deduplication keyed by `request_id`. Reconnecting and
resending a logical mutation with the same or a different `request_id` is a new server request;
only application reconciliation (for example a read of the affected key) can prove whether an earlier
indeterminate attempt committed ([wire protocol §8.1](wire-protocol-v2.md)).

## 6. Timeouts, cancellation, and late responses

### 6.1 Request timeout

When the monotonic deadline expires during send or receive:

1. Classify the call as `transport` (“request deadline expired” or equivalent).
2. **Reset** (close and discard) the affected Worker connection so a late frame cannot be delivered
   to a subsequent call on the same socket.
3. Mutation outcome:
   - `bytes_sent == 0` → `rejected` (after exhausting the §5 zero-byte retry, if any);
   - `bytes_sent > 0` → `indeterminate`.
4. Pipeline: mark unresolved positions from the first incomplete response onward using the same
   bytes-sent / mutation rules as today (earlier validated successes remain successful).

### 6.2 Response after the deadline

Wire protocol v2 has no way to cancel server-side work. A response that arrives after the client
deadline:

- **must not** be treated as success for the timed-out call;
- **must not** be left unread on a reused connection (hence §6.1 connection reset);
- for mutations with `bytes_sent > 0`, leaves the application in `indeterminate` (reconcile via read
  or application idempotency).

Daemon policy for work already admitted on the server is defined by the server model (admitted
durable mutations are not cancelled by client disconnect **or** by daemon
`--request-timeout-ms` / `--idle-timeout-ms` connection reset); clients cannot observe
cancellation acknowledgements in v2. The daemon may still close the TCP connection when a
partial frame, in-flight request or stalled output exceeds `--request-timeout-ms` (abuse counter
`abuse_request_timeout_closed`); Store execution already underway continues to a classified
completion. Cold reads in flight are cancelled via the connection’s read-cancellation epoch
when the socket is closed. See [durable TCP daemon](../operations/durable-tcp-daemon.md).

### 6.3 Application cancellation (async runtimes)

Python `AsyncClient` and Ruby `AsyncClient`: if the awaiting task is cancelled, the implementation
must poison/reset the Worker connection and not reuse buffered bytes for another logical request.
Any future async SDK has the same obligation. Outcome classification for in-flight mutations
follows §6.1 using `bytes_sent`. After any request bytes were written, official async clients
return an indeterminate `MutationResult` / pipeline slot with `reconcile_first` rather than
leaving the application with only a bare cancel exception (`CancelledError` / `Async::Stop`).
Classification must complete before any await that can re-raise cancel during connection
poison/reset (e.g. Python `wait_closed`), so a second cancel cannot erase indeterminate
polarity or fall through to known-unsent (`bytes_sent=0` / `rejected`).
The same per-slot polarity applies when an official client’s outer request deadline kills
in-flight pipeline / batch I/O after send may have occurred: return classified slot vectors,
not a bare transport error that advertises `same_request`. Multi-Worker `execute_batch` outer
cancel has the same sibling-preservation rule as §5: return the classified slot vector
(completed Worker groups kept; in-flight groups classified) rather than aborting with a bare
cancel that discards committed siblings.

### 6.4 Connect timeout

Connect timeout failure is `unavailable` or `transport` before any session exists. It is never a
mutation `indeterminate`.

### 6.5 Per-call request timeout override

Official clients may accept an optional per-call request timeout that **overrides**
`ClientConfig.request_timeout` for that logical operation only:

- C++: `RequestOptions{.timeout = 50ms}`
- Python: `timeout=0.05`
- Perl: `timeout => 0.05`
- Go: `CallOptions{Timeout: 50 * time.Millisecond}`
- Ruby: `timeout:` keyword
- Erlang: timeout option map

Rules:

1. Omitted / unset → use the configured default.
2. Explicit non-positive override → `invalid_argument` before send.
3. Applies to `get` / `put` / `erase` / `ping` / `backup` / `execute_pipeline` / `execute_batch` /
   `execute_worker_pipelines`. Does **not** override connect timeout or bootstrap `INIT` /
   `BIND_WORKER`.
4. Automatic retry (§5) reuses the **same absolute monotonic deadline**, not a fresh budget.
5. `execute_batch` / `execute_worker_pipelines` use one shared deadline for the whole call across
   Workers.

## 7. Pipeline positional outcomes

| Outcome | Meaning |
| --- | --- |
| `succeeded` | Valid matching `OK` (mutation value empty). |
| `failed` | Known unsent, explicit non-internal server rejection, or failed read with no mutation ambiguity for that position. |
| `indeterminate` | Mutation bytes may have reached the server without a trustworthy definitive response, `INTERNAL_ERROR`, or non-empty mutation `OK` value; unresolved suffix after disconnect uses the bytes-sent rule. |

No automatic pipeline retry (§5).

## 8. Unhealthy client

The client becomes **unhealthy** when:

- `worker_count` or `routing_epoch` on a response disagrees with the session; or
- `WRONG_OWNER` / unexpected owner metadata appears on a bound Store operation; or
- `NOT_BOUND` appears after a successful bind on that connection.

Unhealthy clients fail subsequent operations with `unavailable`. Applications must construct a new
client. Protocol v2 defines no online rebalance.

## 9. Conformance

An official SDK conforms when:

1. It passes the shared wire golden fixtures and the cross-SDK interop matrix
   (`scripts/test-sdk-interop.sh`). Secure-profile admission also requires the focused smoke
   (`scripts/test-secure-profile-interop.sh`) once the language is in that matrix.
2. Mutation and pipeline outcomes match §3 and §7 for disconnect, zero-byte send, non-empty
   mutation `OK`, and `INTERNAL_ERROR`.
3. Request deadlines use a monotonic clock and reset the connection on expiry (§1, §6).
4. Automatic retries match §5 exactly (no extra silent retries).

Language-specific API sugar is allowed; silent divergence in outcomes or retry counts is not.

### Network-security lifecycle amendment

[ADR 0041](../adr/0041-bounded-network-security-lifecycles.md) permits bounded output-drain
expiry and closure of over-quota denied request streams. No cancellation acknowledgement is
introduced: sent mutations without a trustworthy definitive response remain indeterminate.
