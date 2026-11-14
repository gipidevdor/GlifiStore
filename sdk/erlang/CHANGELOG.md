# Changelog

## Unreleased

- Package a normalized tracked-source archive, then extract and compile it outside the checkout;
  Hex publication remains a separate release action.
- Compile that extracted archive into an isolated `ERL_LIBS` tree for secure-profile interop.
- Make the interop escript assert structured `permission_denied` and `overloaded` mutation outcomes
  in the all-SDK secure-profile prefix/quota matrix, with a bounded burst for quota exhaustion.
- In installed-artifact interop mode, use the configured Erlang code path and fail if the client
  beam resolves inside the repository.
- BACKUP validate-response failure (mismatched `request_id` / metadata) enriches
  `indeterminate` / `reconcile_first` with `bytes_sent=frame_len`. Litmus:
  `set_wrong_request_id` after connect → BACKUP asserts `bytes_sent > 0`.
- BACKUP wire `INTERNAL_ERROR` / receive-loss / crash / timeout stamp `bytes_sent`
  to the request frame length (not `0` / known-unsent). Litmus: FakeServer
  `internal_error_on_backup` asserts `bytes_sent > 0`.
- Post-exchange mutate status / empty-OK-value / receive-loss paths stamp
  `bytes_sent` to the request frame length (match C++/Perl). Litmus: PUT wire
  `INTERNAL_ERROR` asserts `bytes_sent > 0`.
- `execute_batch` / `execute_worker_pipelines`: connect/rebind failure stamps only
  that Worker's planned slots (`failed` / `rejected`, `bytes_sent=0`) and still
  fans out siblings — full slot vector, not bare `{error}`. Litmus: Worker-1
  `fail_rebind` after conn kill → Worker-0 PUT succeeds + Worker-1 rejected.
- I/O-child crash on pending BACKUP enriches `indeterminate` / `reconcile_first`
  like the outer-deadline path (not bare transport / `same_request`).
- Outer pipeline / fanout deadline and I/O-child crash return per-slot classified
  vectors (mutations `indeterminate` / `reconcile_first`) instead of bare
  `{error, transport}` or all-`failed` + `same_request`. Plan metadata retained on
  pending and fanout children. Litmus: hold-on-PUT pipeline and worker-pipeline
  timeout.
- **OTP supervision:** `glifistore_conn_sup` owns one `temporary` child per Worker with
  intensity 0 (never auto-restart). Conn crashes are monitored; the next request
  explicitly `replace_conn` + re-`INIT`/`BIND_WORKER` and verifies epoch/count (fail-closed).
  Permanent restart is rejected so epoch changes cannot be masked.
- **Concurrency:** `glifistore_client` is a coordinator; normal I/O is delegated via
  `spawn_monitor` so concurrent callers targeting different Workers proceed in parallel.
  Each `glifistore_conn` still serializes its stream. Fan-out uses monitors, deadline
  timers, and safe late-message handling (no unmonitored `spawn/1`).
- Request IDs remain allocated only inside the client gen_server (wrap
  `16#FFFFFFFFFFFFFFFF → 1`). Wire `request_id` stays correlation-only.
- Synchronous `close/1` drains in-flight work after rejecting new requests.
- Metadata mismatch (`routing_epoch` / `worker_count`) marks the client unhealthy.
- Connect uses `gen_server:start/3` so bootstrap failure cannot kill the caller via link.
- CT: `glifistore_concurrency_SUITE` (including `conn_process_crash_then_reconnect`) plus
  extended fake server barriers.
- Benchmark harness measures single/multi caller, same vs different Workers, and pipeline APIs
  with p50/p95/p99.

## 0.1.0

- Initial OTP client: wire protocol v2 codec, structured errors, pipelines, batch,
  `execute_worker_pipelines`, TLS 1.3 connect options, monotonic deadlines, per-call timeouts,
  interop CLI, and golden fixtures.
- Benchmark harness (`benchmarks/client_benchmark.escript`) and
  `scripts/benchmark_erlang_client.sh` with sequential + concurrent matrices.
- `glifistore_version:version/0` exported for packaging and lockstep checks.
- Synchronous `close/1`, exported `permission_denied/1`, pipeline disconnect classification
  aligned with Go/Python (`bytes_sent > begin_offset`), and TLS send via `ssl:send/2`
  (OTP has no timeout arity).
- Hot-path: iolist pipeline send (no forced contig copy), O(1) response slots via `array`,
  O(n) batch frame offsets, selective-receive multi-worker fan-out.
- Benchmark escript uses `+S 4:4` so concurrent `execute_worker_pipelines` can exercise
  multiple schedulers (was `+S 1:1`, which hid fan-out gains).
