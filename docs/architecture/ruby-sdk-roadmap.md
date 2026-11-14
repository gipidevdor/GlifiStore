# Ruby SDK roadmap

**Roadmap** · the implemented official Ruby SDK under `sdk/ruby` · reviewed 2026-08-26

The source-tree boundary of the native Ruby client and the gates it still has to pass. The client
is implemented; this page is not a proposal to create it.

Observable behavior is governed by [client semantics v1](../spec/client-semantics-v1.md), with
framing and routing defined by [wire protocol v2](../spec/wire-protocol-v2.md). Ruby-specific usage
belongs in the [package README](../../sdk/ruby/README.md).

## Current implementation

| Capability | Source-tree state |
| --- | --- |
| Wire codec | 40-byte request/response headers, canonical field checks, frame limits and binary-safe strings |
| Worker routing | FNV-1a 64-bit by default; keyed SipHash-2-4 from the strict extended `INIT` identity |
| Session | `INIT`, one `BIND_WORKER` connection per Worker, `TCP_NODELAY`, routing epoch/count checks |
| Sync API | `get`, `put`, `erase`, `ping`, `backup`, `execute_pipeline`, `execute_batch` |
| Async API | Optional `GlifiStore::AsyncClient` with the same backup/data operations using the `async` gem |
| Failure model | Structured errors, monotonic deadlines, at-most-one permitted retry and indeterminate mutation outcomes |
| TLS | Opt-in TLS 1.3, CA/hostname verification, optional mTLS, fail-closed configuration |
| Packaging | Gemspec, package verification script and release checklist in `sdk/ruby/PACKAGING.md` |
| Evidence | Unit/fake-server tests, shared fixtures, cleartext/TLS interop and benchmark harness |

The keyed-routing path strictly decodes the extended `INIT` identity, applies the advertised
SipHash-2-4 seed, preserves the routing identity across reconnects, and is covered by protocol
vectors and fake-server client tests. This records source-tree capability, not release or
production-gate closure.

`execute_pipeline` is ordered and single-Worker. `execute_batch` groups requests by Worker, overlaps
the groups and restores caller order; it is not a transaction. Sync calls accept `timeout:`, and a
batch shares one absolute monotonic deadline. Async cancellation poisons the affected connection, so
a late response cannot attach to another request.

## Concurrency contract

| Environment | Contract |
| --- | --- |
| MRI threads | One synchronous `Client` may be shared; each Worker connection is protected by a mutex. |
| `fork` / prefork servers | Construct a new client in the child; never reuse parent sockets. |
| Fiber / `async` | Use `AsyncClient` inside an `Async` reactor; do not mix blocking sync I/O into that reactor. |
| Same Worker | One connection serializes exchanges. |
| Different Workers | Sync batch threads or async tasks may proceed concurrently. |

## Open gates

### Installed-artifact and release evidence

`./scripts/package-ruby-client.sh` must continue to build the gem, install it into an isolated
location and run package checks. A registry release also needs the repository version bump,
checksums and provenance, release notes and post-publish install verification. Source presence is
not proof that the gem is already published.

### Performance evidence

Use `./scripts/benchmark_ruby_client.sh` and the shared SDK harness. Record runtime, server commit,
pipeline depth, Worker and client counts, value size, TLS mode, warmup, repeats and validation.
Compare equal execution models: a Fiber concurrency result and a single synchronous pipeline answer
different questions.

Pure-Ruby optimization is profile-led, in this order:

1. remove avoidable String, Array and result allocations and frame copies;
2. reuse connection-local buffers without weakening ownership;
3. measure mutex, Fiber scheduling, syscall and same-Worker serialization costs;
4. consider a narrow C framing/routing extension only when profiles justify its packaging cost.

A C extension is optional, not part of the correctness definition.

### Shared protocol candidates

Connections per Worker and mutation idempotency are cross-SDK and protocol decisions, not Ruby-only
features. They remain governed by the [shared SDK roadmap](sdk-roadmap.md).

## Evidence commands

```bash
./scripts/test-ruby-client.sh
./scripts/package-ruby-client.sh
./scripts/test-sdk-interop.sh
./scripts/benchmark_ruby_client.sh
./scripts/benchmark_sdk_clients.sh
```

## Non-goals

- wrapping the C++ client through FFI as the default implementation;
- changing wire bytes or retry semantics for Ruby convenience;
- treating `request_id` as server-side deduplication;
- silently falling back to cleartext when TLS is requested;
- sharing sockets created before `fork`;
- calling the SDK production-ready before the server's production evidence gates close.
