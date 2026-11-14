# C++ client API

**Normative** · `GlifiStore::client`, wire protocol v2 · reviewed 2026-09-12

How the current installable C++ client connects, routes, pipelines and reports outcomes over wire
protocol v2, and which of its rules every other SDK must reproduce.

## Connection and routing

`glifistore::client::Client` is the reference implementation of wire protocol v2. It is separate
from the embedded Store and the server runtime: applications link `GlifiStore::client`, while the
client links the small `GlifiStore::wire` codec library and, when TLS is enabled at build time,
`GlifiStore::tls_layer`.

`Client::connect()` performs `INIT`, records the server's Worker count, routing epoch and routing
identity (`routing()`), then opens and binds exactly one connection for every Worker. The default
endpoint is TCP `host:port`; set `ClientConfig::unix_socket_path` to dial AF_UNIX instead
([ADR 0029](../adr/0029-uds-peercred.md)). AF_UNIX is mutually exclusive with TLS. Plain
`GlifiStore/2` selects protocol-v2 FNV-1a; the extended identity selects keyed SipHash-2-4, as
specified by wire v2 and [ADR 0030](../adr/0030-keyed-worker-routing.md).

A mutex serializes traffic on each connection, and calls routed to different Workers can execute
concurrently. A `Client` may therefore be shared between threads, but a single Worker has either one
synchronous request or one ordered pipeline in flight through one client instance.

The lifecycle probes `health()`, `ready()` and `stats()` run on Worker 0. On OK, wire v2 returns
ASCII `GlifiStore/live`, `GlifiStore/ready` and a bounded `GlifiStore/stats` report respectively.
They are distinct from `healthy()`, which is local connection fail-closed state. Orchestrators must
gate traffic on `READY`, not on `HEALTH` alone.

The API is synchronous and supports explicitly bounded pipelines. It does not promise asynchronous
completion, connection pooling beyond one connection per Worker, authentication tokens or automatic
topology rebalance.

Opt-in TLS 1.3 is available through `ClientConfig::tls` (`TlsOptions`: `enable`, `ca_file`,
`cert_file`/`key_file` for mTLS, `server_name`, and the lab escape `insecure_skip_verify`). Hostname
and SNI verification is on by default. TLS requests fail closed with no cleartext fallback
([ADR 0020](../adr/0020-tls-outer-transport.md)).

## Basic use

```cpp
#include <glifistore/client/client.hpp>

auto opened = glifistore::client::Client::connect({
    .host = "127.0.0.1",
    .port = 7379,
});
if (!opened) {
    // inspect opened.error()
}
auto cache = std::move(*opened);

auto written = cache.put("session:42", "payload");
if (!written.committed()) {
    // distinguish written.outcome and inspect written.error
}

auto value = cache.get("session:42");
if (!value) {
    // Portable: value.error().category == "not_found"
    // Also: wire_status, retryability, operation, request_id, worker, …
}
```

Keys and values are bytes. The `string_view` overloads preserve embedded NUL bytes and perform no
encoding conversion. Returned values own their storage.

`PutOptions::expire_at_ns` is an absolute Unix timestamp in nanoseconds; zero disables expiration.
The client does not compensate for clock skew.

## Mutation outcomes

`PUT` and `ERASE` return `MutationResult`, not `Result<void>`, because a TCP disconnect alone cannot
prove whether a server-side mutation linearized. The normative tables for outcomes, portable error
categories, automatic retries and deadlines are in
[client semantics v1](../spec/client-semantics-v1.md)
([ADR 0019](../adr/0019-client-error-retry-timeout.md)); this summary matches that specification.

| Outcome | Meaning | Safe default action |
| --- | --- | --- |
| `committed` | An `OK` response with matching request and routing metadata was received, and a successful mutation response carried an empty value. | Treat the mutation as applied. |
| `rejected` | The mutation request was not sent (including a final zero-byte send failure after the one allowed retry), or the server explicitly rejected it before applying it. | Correct the cause; a retry does not duplicate a known commit. |
| `indeterminate` | Request bytes may have reached the server but no trustworthy definitive response was received, the server returned `INTERNAL_ERROR`, or a mutation `OK` carried a non-empty value. | Reconcile with a read or application idempotency policy before retrying. |

Protocol v2 has no idempotency token. The client automatically retries `GET` and `PING` once after a
transport failure, because they are read-only. It retries a mutation only when zero bytes of its
request were sent. Reconnection repeats `INIT` and `BIND_WORKER` and accepts only the original
Worker count and routing epoch. Request deadlines use a monotonic clock; expiry resets the Worker
connection so a late frame cannot satisfy a later call.

## Structured errors

TCP client failures populate portable fields on `glifistore::Error`
([client semantics §2.1](../spec/client-semantics-v1.md)):

| Field | Role |
| --- | --- |
| `code` | Existing C++ `ErrorCode` |
| `category` | Cross-language name (`not_found`, `transport`, …) |
| `message` | Diagnostic text, not a parse API |
| `wire_status` | Decoded response status when available |
| `bytes_sent` | Request bytes written before failure (`0` = known unsent) |
| `request_id` / `worker` / `routing_epoch` | When known for that attempt |
| `retryability` | Derived hint (`same_request`, `new_attempt`, `reconcile_first`, `never`) |
| `operation` | For example `get`, `put`, `pipeline` |
| `mutation_outcome` | On a failed `PUT` or `ERASE`, and at pipeline mutation positions: `rejected` or `indeterminate`; success uses `MutationResult::committed` or pipeline `succeeded` |

Applications that need portability should key off `category` and `mutation_outcome`, not only
`ErrorCode`. Standalone mutations also expose the outcome on `MutationResult`; when an error is
present, the two agree.

## Pipelines and batches

`execute_pipeline()` sends a bounded sequence without waiting between individual requests, then
returns one positional result per request. The complete pipeline must route to one Worker; use
`worker_for(key)` to group application work before submission. Request key and value spans need to
remain valid only until the synchronous call returns.

A failed outer `Result` means the whole batch was rejected before transmission, for example by
validation, limits or connection admission. Once transmission starts, the call returns the
positional response vector even when the transport fails partway through.

```cpp
const std::array requests{
    glifistore::client::PipelineRequest{
        .opcode = glifistore::client::PipelineOpcode::put,
        .key = key,
        .value = value,
    },
    glifistore::client::PipelineRequest{
        .opcode = glifistore::client::PipelineOpcode::get,
        .key = key,
    },
};
auto completed = cache.execute_pipeline(requests);
```

`execute_batch()` accepts the same request type but may span Workers. It groups requests by
`worker_for`, runs one pipeline per non-empty Worker (concurrently when more than one Worker is
involved) and returns responses in the caller's original order. It is not atomic: after admission,
Workers fail independently. The per-Worker `maximum_pipeline_requests` and `maximum_pipeline_bytes`
still apply, and a pre-admission validation failure rejects the whole batch with an outer `Result`
error. One shared request deadline covers the whole batch call.

`execute_worker_pipelines()` takes a vector of length `worker_count()`: index `i` is the pipeline
for Worker `i`, and empty slots are skipped. Every key in slot `i` must route to Worker `i`. It
returns one response vector per Worker, with the same non-atomic fan-out and shared deadline as
`execute_batch`.

An optional `RequestOptions{.timeout = …}` overrides `ClientConfig::request_timeout_ms` for a single
`get`, `put`, `erase`, `ping`, `backup`, `execute_pipeline`, `execute_batch` or
`execute_worker_pipelines` call. Automatic retries reuse the same absolute monotonic deadline.
Connect and bootstrap stay on the configured defaults.

```cpp
auto completed = cache.execute_batch(mixed_worker_requests);
auto quick = cache.get(key, {.timeout = std::chrono::milliseconds{50}});
```

### Limits and depth

The default limits are 256 request frames and 1 MiB of aggregate encoded request data, controlled by
`ClientConfig::maximum_pipeline_requests` and `maximum_pipeline_bytes`; each frame must also satisfy
`maximum_frame_bytes`. The request timeout covers sending and receiving the whole batch.

A pipeline holds its Worker's connection mutex for its duration, while other Workers remain
independent. Large depths improve throughput but also increase batch-completion latency and
head-of-line blocking. Start around 32–64 `PUT`/`GET` pairs (64–128 request frames) per Worker and
tune against the application's tail-latency objective; this is guidance, not a universal optimum.

### Pipeline outcomes

`PipelineResponse::outcome` has per-position transport semantics:

| Outcome | Meaning |
| --- | --- |
| `succeeded` | A valid matching `OK` response was received; GET data is owned by `value`. |
| `failed` | A read failed, or a mutation is known unsent or explicitly rejected. |
| `indeterminate` | Mutation bytes may have reached the server without a trustworthy definitive response, or the server reported `INTERNAL_ERROR`. |

The client does not retry a pipeline automatically. Earlier valid responses remain successful after
a later disconnect, and unresolved mutations and reads are classified independently. This avoids
both silently duplicating mutations and discarding outcomes the server already proved.

## Online backup

`Client::backup(destination)` issues wire `BACKUP` (opcode 10) on Worker 0. The destination path is
UTF-8 and must name an empty directory, which the server creates as needed. The call is online and
**fenced**, not zero-impact hot I/O: the daemon briefly pauses Store admissions while it copies the
durable catalog. Under the secure profile it requires the `admin` capability. On success the owned
response bytes are a bounded ASCII report containing `status=ok` plus file and byte counts. Official
SDKs expose the same typed `backup` helper, also fenced and not hot.

## Failure and lifecycle

- Frame size, canonical reserved fields, request ID, Worker owner, Worker count and routing epoch
  are validated on every response.
- A changed routing epoch or Worker count, or an inconsistent owner, makes the client unhealthy;
  constructing a new client is required because protocol v2 defines no online rebalance.
- A transient socket failure closes only the affected Worker connection. Its next safe operation
  attempts a lazy reconnect.
- `close()` is thread-safe and idempotent. Calls begun concurrently with close may finish first;
  later calls fail with `unavailable`.
- Moving a client transfers ownership. A moved-from client is closed for API purposes.
- Hostname resolution uses the platform resolver. The connect timeout applies to socket connect, not
  to time spent inside a blocking system resolver.

## Cross-language SDK contract

The C++ client and the canonical fixtures define behavior to reproduce, not a C ABI to wrap. Python,
Perl, Go, Ruby and Erlang clients must implement the same wire contract and the same mutation and
pipeline outcome rules natively, so each runtime retains its normal cancellation, scheduling,
packaging and binary-data conventions. Outcome classification is part of the public contract: a
zero-byte final send failure is `rejected`, and a successful mutation response with a non-empty
value is `indeterminate` (in a pipeline, unresolved from that position). Concurrency models may
differ by language, but observable request and response semantics must not.

| Runtime | Intended public shape | Concurrency model |
| --- | --- | --- |
| C++ | Implemented reference client | Synchronous, thread-safe, one connection per Worker |
| Python | Native package under `sdk/python` (`Client` + `AsyncClient`) | Sync: one locked connection per Worker; async: one `asyncio.Lock` per Worker |
| Perl | Native module under `sdk/perl` using byte strings | Synchronous handles; one client per process or thread (event-loop adapter later) |
| Go | Native module under `sdk/go` (`client` + `protocol`) | Synchronous, goroutine-safe, one connection per Worker; `ExecuteBatch` fans out per Worker |
| Ruby | Implemented under `sdk/ruby` (`Client` + optional `AsyncClient`) | Sync: per-Worker mutex; async: Fiber + `async` gem; one client per forked worker process |
| Erlang | Implemented under `sdk/erlang` (`glifistore` OTP app) | Client `gen_server` + one connection process per Worker; `execute_batch` / `execute_worker_pipelines` fan out |

Every SDK must consume the canonical request/response corpus under `tests/fixtures/`, use unsigned
little-endian fields exactly, distinguish indeterminate mutations, and pass the same malformed-frame
and routing vectors. A thin FFI wrapper around the embedded C++ client is not the default design: it
would complicate runtime integration and distribution while hiding semantics that the 40-byte
protocol makes straightforward to implement safely.

Remaining client work, such as ops metrics and an optional Ruby C extension, lives in the
[SDK roadmap](../architecture/sdk-roadmap.md) and the
[Ruby SDK roadmap](../architecture/ruby-sdk-roadmap.md). Secure-profile follow-ons (mTLS principals,
authorization) are tracked in the [security roadmap](../security/roadmap.md). OpenBSD LibreSSL CI
and the [TLS performance note](../security/tls-performance.md) are done.

## Performance interpretation

`glifistore_client_benchmark` measures only the public reference client against an already-running
daemon. It validates every positional PUT/GET response and supports `concurrent`, `sequential` and
mixed-owner `batch` execution. Keys are assigned through the connected client's negotiated routing
identity, so keyed SipHash runs do not silently use an FNV workload.

```bash
build/macos-release/glifistore_client_benchmark --port 7379 --workers 4 \
  --ops 100000 --pipeline 8 --warmup 1 --repeats 7 --execution concurrent
```

The cross-SDK harness includes one C++ concurrent row per Worker/depth combination. The other C++
modes remain available for focused experiments without multiplying the default matrix.

With `--client-api`, the public-client benchmark measures alternating `PUT`/`GET` calls through one
shared `Client`; `INIT`, binding, allocation of client threads and server startup remain outside the
timed region. `--latency` records each synchronous API call.

```bash
./scripts/dev.sh benchmark-server --client-api --ops 100000 \
  --workers 4 --clients 4 --latency --warmup 1 --repeats 5
```

`--client-pipeline N` benchmarks batches of `N` ordered `PUT`/`GET` pairs through the public
pipeline API. `N=32` therefore submits 64 request frames and stays within the default 256-frame
limit.

```bash
./scripts/dev.sh benchmark-server --client-pipeline 32 --ops 100000 \
  --workers 4 --clients 4 --warmup 1 --repeats 5
```

Raw wire `--pipeline N` pre-encodes its frames before timing, while the public client includes API
validation, request encoding, owned GET results and per-request outcome construction. Compare them
to quantify end-to-end SDK cost, not transport alone. Pipeline depth changes the throughput and
latency operating point, so compare only equal depths and workloads.
