# GlifiStore Ruby client

The `glifistore` gem, module `GlifiStore`, is a native Ruby client for GlifiStore wire protocol v2.

## Guarantees

- Opens one TCP connection per Worker, with `TCP_NODELAY`.
- Routes each binary key to its owner Worker as
  [ADR 0030](../../docs/adr/0030-keyed-worker-routing.md) specifies: a plain `GlifiStore/2` INIT
  identity means canonical FNV-1a 64-bit; the extended INIT identity selects SipHash-2-4.
- Implements [client semantics v1](../../docs/spec/client-semantics-v1.md), with `committed`,
  `rejected` and `indeterminate` mutation outcomes.
- Retries at most once automatically, for safe reads and zero-byte mutations.

## Example

```ruby
require "glifi_store"

config = GlifiStore::ClientConfig.defaults
config.port = 7379
client = GlifiStore::Client.connect(config)

result = client.put("session\x0042".b, "payload".b)
raise result.error unless result.committed?

value = client.get("session\x0042".b)
client.close
```

## Install

The client needs Ruby 3.2 or later. From this source tree, load it with `-Ilib`:

```bash
cd sdk/ruby
ruby -Ilib -e 'require "glifi_store"; puts GlifiStore::VERSION'
```

The license is BSD-3-Clause.

## Configuration

`GlifiStore::ClientConfig.defaults` returns a configuration to adjust, as the example does with
`port`. Besides TCP, the client can dial an AF_UNIX socket.

## TLS

Connections are cleartext TCP by default: treat the server as loopback, private network or sidecar.
TLS 1.3 is opt-in through `ClientConfig#tls`, with CA, mTLS and hostname verification, as in the Go,
C++, Python, Perl and Erlang clients ([ADR 0020](../../docs/adr/0020-tls-outer-transport.md)). It
fails closed, with no cleartext fallback. Enable it on the configuration:

```ruby
config.tls = true
config.tls_ca = "/path/to/ca.pem"
config.server_name = "glifistore.example"
# config.cert_file / config.key_file for mTLS
```

See the [security roadmap](../../docs/security/roadmap.md).

## Errors and mutation outcomes

`put` and `erase` return a result that answers `committed?` and carries `error`; the example raises
that error unless the write committed.

## Pipelines and batches

Deep pipelines, `execute_batch` and `execute_worker_pipelines` let Workers overlap; prefer them for
throughput.

## Backup and lifecycle probes

`Client#backup(destination, timeout: ...)` and `AsyncClient#backup` issue wire `BACKUP` and return
the bounded ASCII report. The destination is a new, empty server-side path. BACKUP is an admin
operation under secure authz and an online fenced copy, not a zero-fence hot snapshot. An ambiguous
failure requires reconciliation and is never retried blindly.

`Client` and `AsyncClient` expose `health`, `ready` and `stats` for wire `HEALTH`, `READY` and
`STATS`, with an empty key and value; `ping` is also available.

## Concurrency

| Environment | Rule |
| --- | --- |
| MRI threads | One sync `Client` may be shared; each Worker connection is mutex-protected. |
| `fork` (Puma clustered, Unicorn) | Construct a new client in the child. Never reuse parent sockets. |
| Async / Fiber | Use `GlifiStore::AsyncClient` inside an `Async` reactor (`async` gem). |

The optional asynchronous client needs the `async` gem:

```ruby
# gem install async
require "glifi_store/async_client"

Async do
  client = GlifiStore::AsyncClient.connect(config)
  client.get("key".b)
  client.close
end
```

Cancelling or stopping the task poisons the in-flight Worker connection
([client semantics v1](../../docs/spec/client-semantics-v1.md), §6.3).

## Performance

Scale out with one client per prefork process. Publish loopback numbers with:

```bash
./scripts/benchmark_ruby_client.sh
```

The cross-SDK `./scripts/benchmark_sdk_clients.sh` harness also includes sequential and per-Worker
threaded Ruby rows when `RUBY` resolves to version 3.2 or newer. Older system Rubies are reported as
skipped rather than benchmarked outside the gem's supported runtime contract.

## Limitations

- The client does not combine AF_UNIX with TLS: it refuses that configuration fail-closed.
- An optional C extension for framing remains a
  [Ruby SDK roadmap](../../docs/architecture/ruby-sdk-roadmap.md) item, to be measured first.

## Development

From the repository root, run the tests:

```bash
./scripts/test-ruby-client.sh
```

| Path | Role |
| --- | --- |
| `lib/glifi_store/protocol.rb` | Wire codec, FNV and SipHash routing |
| `lib/glifi_store/error.rb` | Structured errors and outcome types |
| `lib/glifi_store/client.rb` | Synchronous TCP, AF_UNIX and TLS client |
| `lib/glifi_store/async_client.rb` | Async reactor client, with the same surface as the synchronous one |
| `lib/glifi_store/tls.rb` | TLS 1.3 context and wrap helpers |
| `exe/glifistore-interop` | Interop CLI for `scripts/test-sdk-interop.sh` |
| `test/fixtures/` | Vendored wire goldens |

[Ruby client packaging](PACKAGING.md) describes the packaging check, RubyGems publication and the
version-bump checklist.
