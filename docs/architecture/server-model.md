# Server model

**Descriptive** · `glifistored` 0.1.0, paired Reader–Writer runtime

How `glifistored`, the network process around the embedded Store, owns connections, routes keys,
serves reads, admits mutations and bounds its memory. The client-implementable contract is
[wire protocol v2](../spec/wire-protocol-v2.md).

## Runtime and transports

The sole data-plane runtime in 0.1.0 is the paired Reader–Writer model
([ADR 0031](../adr/paired-reader-writer-shards.md)): one Reader (Reactor) and one serial Writer per
shard pair, connected by bounded SPSC lanes. There is no second selectable daemon runtime and no
dual-select switch. The lab-only volatile prototype under `src/experimental/` is not linked into
`glifistored`.

The initial transport is non-blocking IPv4 TCP. Optional TLS 1.3 wraps the same protocol when
`--tls-cert` and `--tls-key` are set ([ADR 0020](../adr/0020-tls-outer-transport.md); LibreSSL on
OpenBSD). With `--tls-port`, one process may expose cleartext and TLS on distinct ports; without it,
TLS configuration makes `--port` TLS-only. Optional Unix-domain sockets (`--unix-socket`,
[ADR 0029](../adr/0029-uds-peercred.md)) use the same protocol and connection state. Optional
`--unix-peercred` maps the OS uid to `--authz-map` principals (`unix:uid=N`); it is not a TLS
replacement.

The wire protocol, the Manifest and wire `STATS` keep Worker wording (`worker_count`, owner Worker
id) for 0.1.x compatibility; the Worker count is the shard-pair count. `--shard-pairs` is the
canonical flag, and `--workers` is its 0.1.x alias.

## Shard pairs

Each shard pair is an independent ownership domain:

```text
Reader / Reactor  ──GET──>  ReadGeneration (immutable, adopted once per turn)
        │
        └── mutation SPSC ──> Writer (serial mutator) ── completion SPSC ──> Reader
```

- The Reader is the readiness-driven sole owner of the pair's sockets, input and output connection
  buffers and local `ReadGeneration` pointer. It never mutates the pair's Index or Segment state.
- The Writer is the sole mutator of the pair's active Segment, sequences and published read
  generation: the only data path that appends to its Segments or publishes a new `ReadGeneration`.
  Mutations and durable cold-read refresh requests reach it only through the pair's bounded SPSC
  lanes (`PairWriterPool`).
- There is no second selectable daemon runtime. The former volatile prototype was removed from the
  tree; the ADR 0036 laboratory under `src/experimental/` is not linked into `glifistored`.

Public embedded `Store::get` returns an owning value
([ADR 0009](../adr/0009-public-read-ownership.md)). Daemon GET is a separate path: it borrows from
the Reader-local `ReadGeneration` for the response lifetime and must not be treated as the public
API contract.

## Readiness and binding

The readiness backend is chosen at compile time: edge-triggered `epoll` on Linux, `kqueue` with
`EV_CLEAR` on macOS, FreeBSD and OpenBSD. Every accepted socket is non-blocking and close-on-exec.
Read and write handlers drain the socket until `EAGAIN`.

On Linux the server may create one `SO_REUSEPORT` listener per Reader; macOS, FreeBSD and OpenBSD
use one public acceptor. Initial placement is temporary. After `INIT`, the client sends
`BIND_WORKER`. If the selected owner differs from the temporary Reader, the source removes the
descriptor from its poller and moves the `SocketHandle` with its buffered connection state through a
bounded handoff queue. The destination registers the same descriptor in its poller and becomes its
sole owner. The kernel TCP connection is neither recreated nor copied.

## Routing

After binding, `GET`, `PUT` and `ERASE` compute `hash(key) % worker_count` (the shard-pair count).
A request executes only when that owner is the bound pair. A mismatched key receives `wrong_owner`
with the correct owner id and routing epoch; it is never forwarded through another pair. Readers use
a private owner-checked path and do not take the public Store API's per-Worker mutex for ordinary
daemon traffic.

## Reads

Volatile GETs and durable hot-generation hits stay on the owner Reader, against the adopted
`ReadGeneration`. A durable cold miss is split. The Reader resolves the exact `RecordRef` and its
generation pin from the published generation, then submits the prepared read to the pair's bounded
cold-read SPSC lane. That consumer reads the file, validates the CRC and materializes the value
without a Worker or catalog lock. The completion returns through the pair's completion path, where
the Reader publishes the response; a closed or reused connection is rejected by its
`(slot, generation)`.

A connection may have only one cold read outstanding. The Reader stops executing that connection's
later frames until the completion arrives, so protocol v2 responses stay in strict request order
without a reorder buffer. It may keep draining socket bytes up to the configured input watermark, so
FIN, RST and overflow remain observable. Other connections on the same Reader continue normally.

## Mutations

`PUT` and `ERASE`, volatile and durable, use one bounded FIFO SPSC mutation lane per shard pair. The
Reader copies only the request's key and value into an owning mutation arena slot; no `RecordRef`,
Segment, file handle or borrowed protocol span crosses this boundary.

Successful enqueue is the daemon's admission and order point, not the storage visibility point. The
Writer drains the lane serially, and the Store's persistence v1 state machine keeps the actual
visibility and durable commit points. A completion returns only after the selected acknowledgement
policy completes. A connection may keep up to 32 adjacent `PUT` and `ERASE` requests in flight
(`kMaximumMutationWindow`, [ADR 0037](../adr/0037-shard-execution-token-flat-combining.md)); the
lane and the completion path are both FIFO, so responses leave in request order. Any other opcode
closes the window: the Reader stops executing that connection's later frames until the window's
completions arrive, and a following `GET` adopts a read generation that contains them or receives
`overloaded`. Independent pairs share no request lock or queue. In `durable_group`, the Writer owns
micro-batch aggregation for its pair; there is no configurable count of competing mutators per
shard.

In durable-sync storage, a successful mutation response is encoded only after the Store crosses the
durable commit point and publishes coherent in-memory state. A disconnect between commit and
response delivery is an indeterminate client outcome, as defined by the
[durability and recovery contract](durability-recovery.md).

## Protocol summary

[Wire protocol v2](../spec/wire-protocol-v2.md) is normative; this summary is not exhaustive.

Frames use explicit little-endian fields and never decode persisted bytes through C++ object layout.
A protocol version 2 request has a 40-byte header:

```text
u32 frame_size
u16 version
u8  opcode
u8  flags
u64 request_id
u32 key_size
u32 value_size
u64 expire_at_ns
u32 target_worker
u32 reserved
key bytes
value bytes
```

`expire_at_ns` is an absolute Unix-epoch timestamp in nanoseconds for `PUT` and must be zero for
other opcodes. `target_worker` is meaningful only for `BIND_WORKER`, where it carries a real owner
id; every other opcode must set it to `kNoWorker`. Encoders and decoders reject non-canonical
opcode-specific fields. The daemon supplies its own current wall-clock timestamp to Store reads.

A response header is also 40 bytes:

```text
u32 frame_size
u16 version
u16 status
u64 request_id
u32 value_size
u32 owner_worker
u32 worker_count
u32 reserved
u64 routing_epoch
value bytes
```

The recommended session is `INIT`, one `BIND_WORKER`, then pipelined Store operations. Version 2
also permits `PING` before initialization and a repeated `INIT`; binding requires initialization
and happens once. `INIT` returns protocol and routing metadata. A Store command before binding
receives `not_bound`; a misdirected key receives `wrong_owner`, and the client retries on the
correct bound connection. `request_id` correlates pipelined responses. Frames are length-checked
before spans are formed; the current maximum frame is 2 MiB.

## Connection identity

Poller events carry a packed `(slot, generation)` token, not a bare file descriptor. Closing a
connection increments its generation, so a delayed event or a late Writer or cold-read completion
cannot target a new connection that reuses the same descriptor or slot.

## Backpressure

Every buffer and queue is bounded: per-connection input and output buffers, each Reader's one-time
connection handoff queue, every per-pair mutation SPSC, every cold-read SPSC and every completion
path. Queue capacity is rounded up to a power of two at startup; overload never becomes unbounded
memory growth.

Input frames use a consumed-prefix cursor: completed frames advance the cursor without moving the
buffered suffix, and an append compacts the suffix only when it would otherwise reallocate or cross
the byte watermark. Contiguous output applies the same sliding-cursor rule after partial socket
writes. Aggregate `STATS` expose the residual copy pressure as `input_buffer_compactions`,
`input_buffer_bytes_moved`, `output_buffer_compactions` and `output_buffer_bytes_moved`.

Mutation admission has both a request-count limit and an owned-byte limit, so maximum-size payloads
cannot multiply up to the count limit. A cold-read or mutation admission failure returns
`overloaded` and enqueues no work. Exceeding an input or output byte watermark closes the offending
connection.

The mutation lane also has a configurable wait deadline. A task that expires before Store entry
returns `overloaded` and is known not committed; a task that has entered the Store is never
cancelled.

Handoff and accept failures:

- A full handoff queue restores the connection on the source Reactor, returns wire `overloaded` for
  that `BIND_WORKER` when the socket can still flush, then closes the connection. There is no silent
  success ACK or EOF.
- Destination connection-table exhaustion after a successful enqueue, and an adopt poller or
  `SO_SNDBUF` failure, likewise send `overloaded` on a best-effort basis and then close: the
  buffered BIND OK must not die with the socket.
- Accept floods with no prior bind still drop silently.
- A single `accept` or `configure_*` failure for one peer does not stop the Reactor executor; the
  listen loop retries on the next readiness event, the isolation a rejected TLS handshake already
  used.

## Telemetry

`Server::pair_writer_stats()` returns per-pair snapshots: current and peak queue records and owned
bytes; admitted, rejected, expired and completed counts; queue-wait time; and total Store service
time. These metrics use only short admission and telemetry paths and cannot wait for filesystem
operations.

`Server::durable_batch_stats()` exposes the Store kernel's lock-free per-owner batch snapshot:
pending and committed occupancy, adaptive target, close reasons, failures and the exact duration of
the Segment batch commit operation. These counters complement lane queue and service time instead of
conflating scheduling delay with persistence work.

## Shutdown and health

Graceful stop stops accepting new connections, drains existing connections within
`--shutdown-drain-ms`, drains mutations already admitted before closing the Store, and fails closed
if the shared deadline expires.

Wire `HEALTH` reports process and executor liveness (`started && !failed_`) without requiring Worker
binding. `READY` additionally requires traffic admission, including paired Writer health.

A client disconnect does not cancel admitted storage work: the stale `(slot, generation)` completion
is discarded, and the client must classify the mutation as indeterminate.

## Current scope

The daemon supports the Store's volatile and durable modes. Reader threads are not yet guaranteed to
run on performance cores. Optional executor affinity is strict over the process-allowed CPU set on
Linux; macOS exposes only Mach affinity tags, so its mode is advisory, not a hard performance-core
pin.

The executable validates:

- `INIT`, one-time connection rebinding and wrong-owner rejection;
- TCP lifecycle, native readiness, partial frames, pipelining and large partial output;
- half-close, which drains residual frames and decided ACKs before teardown;
- decided-response drain on a pipelined decode failure, including abuse `OVERLOADED` from
  `read_ready`;
- a partial-frame request timeout that drains prior decided output before close;
- connection generations and graceful stop.

## Server benchmark

`glifistore_server_benchmarks` measures the real loopback TCP protocol with one owner-bound
connection per client. Server startup, `INIT`, `BIND_WORKER`, connection establishment, request
encoding and client thread creation are outside the timed region; every response is decoded and
validated.

Traffic counters report the exact protocol-frame bytes sent into and returned by the server during
the timed region, with ingress, egress and aggregate duplex bytes per second. Memory counters report
current RSS after each sample, its change from the post-setup baseline, and process peak RSS. The
benchmark server and its loopback clients run in one process, so RSS describes the whole benchmark
process, not an isolated daemon.
