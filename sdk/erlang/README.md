# GlifiStore Erlang client

The `glifistore` OTP application is a native Erlang/OTP client for GlifiStore wire protocol v2.

## Guarantees

- Opens one connection per Worker, over TCP or TLS.
- Routes each binary key to its owner Worker as
  [ADR 0030](../../docs/adr/0030-keyed-worker-routing.md) specifies: a plain `GlifiStore/2` INIT
  identity means canonical FNV-1a 64-bit; the extended INIT identity selects SipHash-2-4.
- Follows [client semantics v1](../../docs/spec/client-semantics-v1.md): monotonic request
  deadlines, routing-epoch fail-closed checks and `committed`, `rejected` and `indeterminate`
  mutation outcomes.
- Retries at most once automatically, for reads and zero-byte mutations, within one shared
  monotonic deadline.

## Example

```erlang
{ok, Client} = glifistore_client:connect(#{host => "127.0.0.1", port => 7379}),
#{outcome := committed} = glifistore_client:put(Client, <<"session\00042">>, <<"payload">>),
{ok, Value} = glifistore_client:get(Client, <<"session\00042">>),
ok = glifistore_client:close(Client).
```

## Install

The client needs Erlang/OTP 25 or later and rebar3. On macOS, install both from MacPorts; do not
install OTP through kerl, asdf or Homebrew for this project:

```bash
sudo port install erlang rebar3
```

Make sure `/opt/local/bin` is on `PATH`, so that `erl` and `rebar3` resolve from MacPorts.
[Erlang client packaging](PACKAGING.md) explains how to consume the application before it is
published on Hex.

`glifistore_version:version/0` returns the client version, which must match the repository
`VERSION`. The license is BSD-3-Clause.

## Configuration

`glifistore_client:connect/1` takes a map with `host` and `port`, as in the example. With
`unix_socket_path`, the client dials AF_UNIX instead
([ADR 0029](../../docs/adr/0029-uds-peercred.md)); a `tls` map configures TLS.

## TLS

Connections are cleartext by default. Optional TLS 1.3 needs the standard OTP `ssl` application;
without it, a TLS connect fails closed with `unavailable`, the same soft-exclude pattern as the Perl
client without `IO::Socket::SSL`.

```erlang
Config = #{
    host => "127.0.0.1",
    port => 7379,
    tls => #{enable => true, ca_file => "/path/to/ca.pem", server_name => "localhost"}
},
{ok, Client} = glifistore_client:connect(Config).
```

TLS sends use `ssl:send/2` with deadline checks, because OTP `ssl` has no `send/3` timeout arity.
The root interop harness includes Erlang in both the cleartext and the TLS matrices when OTP and
rebar3 are available.

## Errors and mutation outcomes

A mutation returns a map whose `outcome` key holds its outcome; the example matches `committed`. A
mismatch in routing metadata returns `unavailable` and marks the client unhealthy, fail-closed.

## Pipelines and batches

The client offers `execute_pipeline`, `execute_batch` and `execute_worker_pipelines/2,3`.
`execute_worker_pipelines/2,3` is public, as in the Perl client, so callers can pre-shard work
without the client re-hashing it. Pipeline frames are sent as iodata (`iolist`), which avoids an
extra contiguous copy of the aggregated frames.

## Backup and lifecycle probes

`glifistore_client:backup(Client, Destination[, Opts])` issues wire `BACKUP`; `Destination` is a
non-empty binary naming a new, empty server-side path. Success returns the bounded ASCII report. The
operation requires `admin` under secure authz and is fenced, not zero-fence hot. An ambiguous
failure is reconciled first and never retried blindly.

`health/1,2`, `ready/1,2` and `stats/1,2` match the other official SDKs: on OK they return ASCII
`GlifiStore/live`, `GlifiStore/ready` and a `GlifiStore/stats…` body.

## Concurrency

`glifistore_client` is only a coordinator: configuration, routing metadata, request-ID allocation,
health and epoch, lifecycle. Normal request I/O is delegated to monitored processes that call the
per-Worker `glifistore_conn` `gen_server`. Callers targeting different Workers proceed in parallel
on the BEAM, while each Worker connection still serializes request and response on its single TCP
or TLS stream; this parallelism does not weaken mutation semantics. Many Erlang processes may share
one client safely.

OTP processes are the concurrency model, so the 0.1.x client has no async variant. The client
starts with `gen_server:start/3`, so a bootstrap failure does not kill the caller through a link.

| Rule | Behavior |
| --- | --- |
| Bootstrap and reconnect | Bootstrap opens `worker_count` connections; reconnect repeats `INIT` and `BIND_WORKER` |
| Serialize per Worker | `glifistore_conn` queues exchanges; a pipeline is one atomic `run_pipeline/4` call that sends and collects |
| Parallel across Workers | The coordinator returns `{noreply, …}` while I/O runs off the callback |
| Request IDs | Allocated only inside the coordinator; wrap `16#FFFFFFFFFFFFFFFF → 1`, correlation only on the wire, no server deduplication |
| Fan-out | `spawn_monitor` plus a deadline timer; DOWN, timeout and late messages are handled safely |
| Connection supervision | `glifistore_conn_sup`: one `temporary` child per Worker, intensity 0, no auto-restart |
| Connection crash | Monitored; the next request replaces the connection through `replace_conn`, with INIT and BIND and an epoch and count check |
| `close/1` | Synchronous: rejects new work, drains or fails in-flight requests, stops the connections |

## Performance

From the repository root, against a local `glifistored`, with the same matrix as the Python, Perl,
Go and Ruby clients:

```bash
./scripts/benchmark_erlang_client.sh
# short smoke:
OPS=2000 WARMUP=0 REPEATS=1 ./scripts/benchmark_erlang_client.sh /tmp/erlang-bench-smoke
# meaningful concurrent matrix:
OPS=20000 WARMUP=1 REPEATS=3 CALLERS=8 ./scripts/benchmark_erlang_client.sh
```

The harness reports separately:

- a single caller against multiple callers sharing one client;
- traffic across different Workers against traffic concentrated on one Worker;
- `execute_pipeline`, `execute_batch` and `execute_worker_pipelines`;
- pipeline sizes 1, 8, 32 and 128 and Worker counts 1, 2 and 4, in cleartext and over TLS when
  available.

It measures throughput, p50, p95 and p99 latency, and error and indeterminate counts. The primary
criterion is many Erlang processes sharing one client against different Workers. Artifacts land
under `benchmark-results-erlang-*`. The escript pins `+S 4:4` for cross-machine comparability.

## Limitations

- Bootstrap and reconnect still run on the coordinator, a rare path.
- Same-Worker throughput stays connection-bound by design.
- Worker connections have no permanent auto-restart. This is intentional: it avoids hiding epoch
  changes.
- `unix_socket_path` and TLS are mutually exclusive.

## Development

| Module | Role |
| --- | --- |
| `glifistore_protocol` | Wire codec, FNV-1a routing, golden fixture tests |
| `glifistore_error` | Structured client semantics v1 errors |
| `glifistore_conn` | Per-Worker TCP/TLS connection `gen_server` (serial I/O) |
| `glifistore_conn_sup` | OTP supervisor for Worker connections (`temporary`, intensity 0) |
| `glifistore_client` | Public synchronous API and coordinator (noreply I/O delegation) |
| `scripts/glifistore-interop.escript` | CLI for `scripts/test-sdk-interop.sh` |
| `benchmarks/client_benchmark.escript` | Throughput and latency harness |

Build and test:

```bash
cd sdk/erlang
rebar3 compile
rebar3 ct
rebar3 dialyzer
```

From the repository root:

```bash
./scripts/test-erlang-client.sh
./scripts/package-erlang-client.sh
./scripts/sync-sdk-fixtures.sh   # refresh vendored wire hex fixtures
```

After `rebar3 compile`, run the interop CLI:

```bash
sdk/erlang/scripts/glifistore-interop.escript --port "$PORT" get --key-hex 6161
```

The root interop harness builds the beams and invokes this script when `erl` and `rebar3` are on
`PATH`. [Erlang client packaging](PACKAGING.md) describes the packaging check, Hex publication and
the version-bump checklist.
