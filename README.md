<p align="center">
  <img src="artwork/apple/raster/glifistore-app-icon-128.png" width="72" height="72" alt="GlifiStore logo">
</p>

<h1 align="center">GlifiStore</h1>

<p align="center">
  <strong>A segmented, log-indexed, memory-first key-value store in C++23.</strong>
</p>

<p align="center">
  <a href="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/benchmarks.yml"><img alt="Benchmarks" src="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/benchmarks.yml/badge.svg"></a>
  <a href="CHANGELOG.md"><img alt="Version 0.1.0" src="https://img.shields.io/badge/version-0.1.0-blue"></a>
  <a href="#project-status"><img alt="Architectural prototype" src="https://img.shields.io/badge/status-architectural%20prototype-orange"></a>
  <a href="LICENSE"><img alt="BSD-3-Clause" src="https://img.shields.io/badge/license-BSD--3--Clause-blue"></a>
</p>

![GlifiStore storage engine laboratory](docs/assets/glifistore-hero.png)

GlifiStore stores opaque binary keys and values in one logical key-space. Keys are routed to
independent paired Reader–Writer ownership domains; immutable records are appended to fixed 64 MiB
Segments and an exact-key Index points to the latest visible record. The project can be embedded
through the C++ API or the separately versioned C ABI, or run through the native `glifistored`
daemon and wire protocol v2.

It is a key-value engine, not a SQL or document database. See
[where performance matters](docs/architecture/where-performance-matters.md) before treating raw
engine throughput as application throughput.

## Project status

> [!IMPORTANT]
> **Architectural prototype — not production ready.** Volatile storage, persistence v1, recovery,
> compaction, the paired daemon, secure transport, the C ABI v1 implementation, and native clients
> are present and tested. The first retained cross-release C ABI proof, stable product-release
> compatibility, filesystem/device power-loss certification, signed published artifacts, and a full
> production evidence matrix are not complete. Follow the machine-readable
> [assurance catalog](engineering/README.md) and the derived
> [production-readiness gates](docs/production-readiness.md), not feature presence alone. Agent and
> contributor rules: [AGENTS.md](AGENTS.md).

Implemented surfaces:

| Surface | Current implementation |
| --- | --- |
| Embedded C++ Store | `GET`, `PUT`, `PUT batch`, `ERASE`, TTL, flush, fenced online backup and explicit compaction; volatile and three durable acknowledgement policies |
| C ABI | ABI v1 shared library: version/status, open/close, owning `GET`, `PUT`, `PUT batch` and `ERASE`; volatile and durable-sync only |
| Persistence | Versioned Manifest, Segment, Record and commit-slot formats; bounded recovery and fail-closed validation |
| Daemon | Non-blocking `epoll`/`kqueue`, owner-bound connections, bounded queues, graceful drain and background maintenance |
| Protocol | Wire v2: `INIT`, `BIND_WORKER`, `PING`, `GET`, `PUT`, `ERASE`, `BACKUP`, `HEALTH`, `READY`, `STATS` |
| Security | Optional TLS 1.3/mTLS, capability and key-prefix authorization, quotas, audit events, CRL checks and UDS peer credentials |
| Clients | C++, Python, Perl, Go, Erlang/OTP and Ruby, all using the same fixtures and client-semantics contract |

## Architecture

```text
key ── hash/routing seed ──> owner shard pair ──> exact-key Index
                                                       │
PUT/ERASE ── Reader ── SPSC ── Writer ── append Record ─┤
GET       <── ReadGeneration (Reader-local) <── Segment ┘
```

For 0.1.0, `glifistored` runs only the paired Reader–Writer model ([ADR 0031](docs/adr/paired-reader-writer-shards.md)):
each shard pair has one Reader/Reactor and one serial Writer. After `INIT`, a client binds each
connection to one owner id; if necessary, the daemon hands the socket and its buffered state to the
owning Reader exactly once. Requests for a different owner return `WRONG_OWNER` and are not
forwarded. Mutations leave the Reader through a bounded SPSC lane to that pair’s Writer; GET uses a
local immutable `ReadGeneration` adopted once per event-loop turn. Persistence and wire v2 retain
the compatibility name `worker_count` for the shard-pair count. `--shard-pairs` is the canonical
daemon option; `--workers` is its deprecated 0.1.x alias.

The volatile TCP engine under `src/experimental/` is a **lab-only** prototype (tests and dedicated
benchmarks). It is not installed and is not reachable from `glifistored`. Residual P1 performance
gates are tracked in [paired-shards-plan.md](docs/benchmarks/paired-shards-plan.md).

Authoritative details live in the [architecture specification](docs/spec/architecture.md),
[server model](docs/architecture/server-model.md), and
[persistence v1 specification](docs/spec/persistence-v1.md).

## Quick start

### macOS

```bash
./scripts/bootstrap-macos.sh
./scripts/dev.sh configure
./scripts/dev.sh build
./scripts/dev.sh test
```

`bootstrap-macos.sh` installs CMake and Ninja in a repository-local Python environment. Xcode users
can run `./scripts/open-xcode.sh`; details are in the
[macOS development guide](docs/development-macos.md).

### Linux and BSD

With CMake 3.25+, a C++23 compiler and Ninja:

```bash
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --preset unix-debug
```

Use `macos-release` or `unix-release` for optimized builds. The complete portable workflow,
sanitizers, fuzzing and hardening options are in [docs/development.md](docs/development.md).

### Run the daemon

```bash
./build/macos-debug/glifistored --bind 127.0.0.1 --port 7379 --shard-pairs 4
```

Use the corresponding `build/unix-debug` path on Linux or BSD. The default endpoint is cleartext
loopback with volatile storage. A durable deployment is explicit:

```bash
./build/macos-release/glifistored \
  --profile production \
  --data-dir /private/path/to/glifistore \
  --bind 127.0.0.1 --port 7379
```

Inspect every resolved default and override without opening a Store or listener:

```bash
./build/macos-release/glifistored --profile production \
  --data-dir /private/path/to/glifistore --dump-config
```

The [CLI reference](docs/cli.md) documents configuration precedence, profiles, durable limits,
TLS/mTLS, authorization, maintenance controls, signals and exit codes. The
[durable daemon runbook](docs/operations/durable-tcp-daemon.md) covers deployment and recovery
procedures.

## Storage and durability

The embedded Store and daemon expose four modes:

| Mode | Successful mutation acknowledgement |
| --- | --- |
| `volatile_memory` / `volatile` | Visible in memory; no restart persistence |
| `durable_sync` / `durable-sync` | Per-mutation ordered Record and commit-slot durability boundary |
| `durable_group` / `durable-group` | Same strict acknowledgement, shared across a bounded group |
| `durable_periodic` / `durable-periodic` | Acknowledges before the next periodic durable boundary; bounded loss window |

All durable modes use persistence v1; there is no alternate on-disk format or automatic migration.
Durable resource limits and maintenance policy are runtime configuration, not persisted format.
Reopening uses the persisted Worker topology and routing state.

> [!WARNING]
> Durable mode is not certified on any filesystem/device combination. Remote and user-space
> filesystems such as NFS, SMB, FUSE and overlay filesystems are unsupported. Process-kill and
> syscall-fault tests do not replace reviewed power-loss evidence.

Use the [public C++ API reference](docs/reference/cpp-api.md),
[durability/recovery guide](docs/architecture/durability-recovery.md), and
[platform evidence matrix](docs/architecture/platform-durability-evidence.md) for the exact
acknowledgement and recovery contracts.

The installed shared-library boundary is C ABI v1 (`GlifiStore::abi` or
`pkg-config glifistore-abi`). Its deliberately smaller surface and compatibility limits are in the
[C ABI specification](docs/spec/c-abi-v1.md) and [ABI guide](docs/abi/README.md). The C++ ABI remains
unstable before 1.0, and an ABI contract does not raise the product above architectural-prototype
status.

### Large values in the embedded prototype

A new durable paired Store may opt in with `StoreConfig::enable_blobs = true`. Single-item
`put/get/erase` then support values up to **64 MiB**, with `begin_blob_put` / `open_blob` for
bounded streaming. Blob files are immutable and typed references use an explicit persistence v2
profile; existing v1 Stores are not upgraded. Daemon, SDK and C ABI blob support remain deferred.
See [Large Blob Storage v1](docs/spec/large-blob-storage-v1.md) and its open assurance gate.

## Native clients

Every client implements default FNV wire-v2 routing, monotonic deadlines, bounded ordered pipelines
and the portable `committed` / `rejected` / `indeterminate` mutation model. A pipeline belongs to
one Worker; batch APIs group by Worker and restore caller order. A batch is not a transaction.

| Language | Implementation | Concurrency surface |
| --- | --- | --- |
| [C++](docs/reference/cpp-client-api.md) | Installable `GlifiStore::client` library | Thread-safe Worker connections; pipeline and batch |
| [Python](sdk/python/README.md) | Standard-library sync and `asyncio` clients | Thread-safe sync client; native async client |
| [Perl](sdk/perl/README.md) | Synchronous, binary-safe client | One client per process/thread; concurrent per-Worker pipelines in one select loop |
| [Go](sdk/go/README.md) | Synchronous native module | Mutex per Worker; batch fan-out with goroutines |
| [Erlang](sdk/erlang/README.md) | OTP application | Shareable coordinator and one `gen_server` connection per Worker |
| [Ruby](sdk/ruby/README.md) | Synchronous and optional `async` clients | MRI-thread-safe sync client; Fiber-aware async client |

The packages are present in the source tree but are not promised to be published in every language
registry yet. Run the shared compatibility matrix with:

```bash
./scripts/test-sdk-interop.sh
./scripts/test-secure-profile-interop.sh   # mTLS + authz + keyed + prefix + CRL + quotas
```

The normative behavior is [client semantics v1](docs/spec/client-semantics-v1.md); packaging and
release state are tracked by the [SDK roadmap](docs/architecture/sdk-roadmap.md).

> [!NOTE]
> Keyed SipHash Worker routing ([ADR 0030](docs/adr/0030-keyed-worker-routing.md)) is implemented by
> the daemon and every official SDK (C++ / Python / Perl / Go / Erlang / Ruby): plain `GlifiStore/2`
> keeps FNV-1a; the extended INIT identity selects SipHash-2-4. Secure-profile smoke
> (`scripts/test-secure-profile-interop.sh`, CI) covers mTLS, authz, prefix, CRL, quotas, and keyed
> routing for cpp/python/go plus perl/ruby/erlang when those toolchains are present.

## Performance and engineering evidence

```bash
./scripts/dev.sh test
./scripts/dev.sh asan
./scripts/dev.sh tsan
./scripts/dev.sh benchmark
./scripts/dev.sh benchmark-server --workers 4 --clients 4 --pipeline 32 \
  --executor-affinity --warmup 1 --repeats 5
./scripts/benchmark_sdk_clients.sh
```

Benchmarks validate every response and report throughput; selected harnesses also report latency,
traffic and memory. GitHub-hosted results are regression signals, not absolute product claims.
Publishable comparisons require identical code, workload, routing, durability, dataset, hardware,
affinity and measurement rules. Those rules are defined in the
[benchmark standard](docs/spec/benchmark-standard.md).

Paired-runtime benchmark gates and residual P1 work are kept outside this README in
[docs/benchmarks/paired-shards-plan.md](docs/benchmarks/paired-shards-plan.md). Raw local benchmark
output is intentionally gitignored.

## Manual pages

Installed Runtime builds ship portable mdoc manuals (`man 8 glifistored`, `man 7 glifistore`,
and section-1 pages for offline tools). See [`man/README.md`](man/README.md).

## Documentation

Start with the [documentation map](docs/README.md).

For a browsable C++ reference with class/namespace navigation, header source, and search, see
[building and publishing the Doxygen / GitHub Pages site](docs/development/documentation.md).

The shortest useful paths are:

| Goal | Read |
| --- | --- |
| Understand the system | [Architecture specification](docs/spec/architecture.md) and [code tour](docs/development/code-tour.md) |
| Embed the Store | [C++ API reference](docs/reference/cpp-api.md) |
| Implement or use a client | [Wire protocol v2](docs/spec/wire-protocol-v2.md) and [client semantics v1](docs/spec/client-semantics-v1.md) |
| Operate the daemon | [Operations index](docs/operations/README.md) and [CLI reference](docs/cli.md) |
| Evaluate durability | [Persistence v1](docs/spec/persistence-v1.md) and [evidence matrix](docs/architecture/platform-durability-evidence.md) |
| Review security | [Secure profile](docs/security/secure-profile.md) and [threat model](docs/security/threat-model.md) |
| Trace requirements / hazards / gates | [Assurance catalog](engineering/README.md), [baseline](docs/assurance/engineering-baseline.md), [AGENTS.md](AGENTS.md) |
| Contribute | [Contributing guide](CONTRIBUTING.md) and [test strategy](docs/development/test-strategy.md) |

## Supported platforms

- macOS on Apple Silicon is the primary development platform; macOS Intel remains a target.
- Linux x86-64 and Arm64, macOS, OpenBSD/LibreSSL, and FreeBSD have build/test workflows. The BSD
  VM rows and hosted runners are portability and regression evidence, not filesystem durability
  certification.
- No platform/filesystem row is E3/E4 power-loss certified.

## Security

Cleartext loopback remains the default development posture. The implemented daemon secure profile
requires TLS 1.3, mTLS and a default-deny authorization map. Every official source SDK implements
TLS/mTLS and the extended INIT identity used by keyed SipHash routing; availability in a particular
language runtime still depends on its TLS/toolchain support. This does not certify hostile public or
multi-tenant deployment. Read [SECURITY.md](SECURITY.md) before reporting a vulnerability and use the
[secure-profile runbook](docs/operations/secure-profile-certs.md) for certificates and revocation.

## Contributing and license

Development and review rules are in [CONTRIBUTING.md](CONTRIBUTING.md). GlifiStore is licensed
under BSD-3-Clause; see [LICENSE](LICENSE), [NOTICE](NOTICE), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Licensing policy:
[docs/legal/licensing.md](docs/legal/licensing.md).
