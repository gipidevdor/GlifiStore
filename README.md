<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/glifistore-banner-dark.svg">
    <img src="docs/assets/glifistore-banner-light.svg" width="720" alt="GlifiStore — segmented, log-indexed, memory-first key-value engine in C++23">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/gpicchiarelli/GlifiStore/actions/workflows/ci.yml/badge.svg"></a>
  <a href="#status"><img alt="Architectural prototype" src="https://img.shields.io/badge/status-architectural%20prototype-D9922E"></a>
  <a href="CHANGELOG.md"><img alt="Version 0.1.0" src="https://img.shields.io/badge/version-0.1.0-091A3A"></a>
  <a href="LICENSE"><img alt="BSD-3-Clause" src="https://img.shields.io/badge/license-BSD--3--Clause-091A3A"></a>
</p>

<p align="center">
  <a href="#model">Model</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#durability">Durability</a> ·
  <a href="#clients">Clients</a> ·
  <a href="#evidence">Evidence</a> ·
  <a href="docs/README.md">Documentation</a>
</p>

---

GlifiStore stores opaque binary keys and values in one logical key-space. Each key is routed to an
independent Reader–Writer ownership domain; immutable Records are appended to fixed 64 MiB Segments
and an exact-key Index points to the latest visible Record. Embed it through the C++ API or the
separately versioned C ABI, or run the native `glifistored` daemon over wire protocol v2.

It is a key-value engine, not a SQL or document database. Read
[where performance matters](docs/architecture/where-performance-matters.md) before treating engine
throughput as application throughput.

## Status

> [!IMPORTANT]
> **Architectural prototype — not production ready.** Volatile storage, persistence v1, recovery,
> compaction, the paired daemon, secure transport, the C ABI v1 implementation and native clients
> are present and tested. Not yet complete: the first retained cross-release C ABI proof, stable
> product-release compatibility, filesystem/device power-loss certification, signed published
> artifacts and a full production evidence matrix.
>
> Readiness is decided by the machine-readable [assurance catalog](engineering/README.md) and its
> derived [quality gates](docs/production-readiness.md), never by feature presence.
> Contributor and agent rules: [AGENTS.md](AGENTS.md).

| Surface | Implemented |
| --- | --- |
| Embedded C++ Store | `GET`, `PUT`, `PUT batch`, `ERASE`, TTL, flush, fenced online backup, explicit compaction; volatile and three durable acknowledgement policies |
| C ABI v1 | Shared library: version/status, open/close, owning `GET`, `PUT`, `PUT batch`, `ERASE`; volatile and durable-sync only |
| Persistence v1 | Versioned Manifest, Segment, Record and commit-slot formats; bounded recovery, fail-closed validation |
| Daemon | Non-blocking `epoll`/`kqueue`, owner-bound connections, bounded queues, graceful drain, background maintenance |
| Wire protocol v2 | `INIT`, `BIND_WORKER`, `PING`, `GET`, `PUT`, `ERASE`, `BACKUP`, `HEALTH`, `READY`, `STATS` |
| Security | Optional TLS 1.3/mTLS, capability and key-prefix authorization, quotas, audit events, CRL checks, UDS peer credentials |
| Clients | C++, Python, Perl, Go, Erlang/OTP, Ruby — one fixture set, one client-semantics contract |

## Model

```text
key ── hash/routing seed ──> owner shard pair ──> exact-key Index
                                                       │
PUT/ERASE ── Reader ── SPSC ── Writer ── append Record ─┤
GET       <── ReadGeneration (Reader-local) <── Segment ┘
```

`glifistored` runs one runtime: paired Reader–Writer shard pairs
([ADR 0031](docs/adr/paired-reader-writer-shards.md)).

- **Ownership.** Each pair has one Reader/Reactor and one serial Writer. After `INIT`, a connection
  binds to one owner; the daemon hands the socket and its buffered state to the owning Reader
  exactly once. Requests for another owner return `WRONG_OWNER` and are never forwarded.
- **Mutations** leave the Reader through a bounded SPSC lane to that pair's Writer.
- **Reads** use a Reader-local immutable `ReadGeneration`, adopted once per event-loop turn.
- **Naming.** Persistence and wire v2 keep `worker_count` as the compatibility name for the
  shard-pair count. `--shard-pairs` is canonical; `--workers` is its deprecated 0.1.x alias.

The volatile TCP engine in `src/experimental/` is **lab-only**: built for tests and dedicated
benchmarks, never installed, unreachable from `glifistored`. Residual P1 performance gates live in
the [paired-shards plan](docs/benchmarks/paired-shards-plan.md).

Authority: [architecture specification](docs/spec/architecture.md) ·
[server model](docs/architecture/server-model.md) ·
[persistence v1](docs/spec/persistence-v1.md).

## Quick start

**macOS**

```bash
./scripts/bootstrap-macos.sh
./scripts/dev.sh configure
./scripts/dev.sh build
./scripts/dev.sh test
```

`bootstrap-macos.sh` installs CMake and Ninja in a repository-local Python environment. For Xcode,
run `./scripts/open-xcode.sh`; see the [macOS guide](docs/development-macos.md).

**Linux and BSD** — CMake ≥ 3.25, a C++23 compiler, Ninja:

```bash
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --preset unix-debug
```

Use `macos-release` or `unix-release` for optimized builds. Sanitizers, fuzzing and hardening are in
the [development guide](docs/development.md).

**Run the daemon** — cleartext loopback, volatile storage by default (`build/unix-*` on Linux/BSD):

```bash
./build/macos-debug/glifistored --bind 127.0.0.1 --port 7379 --shard-pairs 4
```

Durability is always explicit. Add `--dump-config` to print every resolved default and override
without opening a Store or listener:

```bash
./build/macos-release/glifistored \
  --profile production \
  --data-dir /private/path/to/glifistore \
  --bind 127.0.0.1 --port 7379
```

Configuration precedence, profiles, limits, TLS/mTLS, authorization, signals and exit codes:
[CLI reference](docs/cli.md). Deployment and recovery:
[durable daemon runbook](docs/operations/durable-tcp-daemon.md).

## Durability

| Mode | A successful mutation is acknowledged when… |
| --- | --- |
| `volatile_memory` / `volatile` | visible in memory; nothing survives restart |
| `durable_sync` / `durable-sync` | its Record and commit slot cross an ordered durability boundary |
| `durable_group` / `durable-group` | the same strict boundary is reached, shared across a bounded group |
| `durable_periodic` / `durable-periodic` | accepted before the next periodic boundary; bounded loss window |

Every durable mode uses persistence v1 — no alternate format, no automatic migration. Resource limits
and maintenance policy are runtime configuration, not persisted format. Reopening uses the persisted
Worker topology and routing state.

> [!WARNING]
> Durable mode is **not certified** on any filesystem/device combination. NFS, SMB, FUSE, overlay and
> other remote or user-space filesystems are unsupported. Process-kill and syscall-fault tests do not
> replace reviewed power-loss evidence.

Contracts: [C++ API](docs/reference/cpp-api.md) ·
[durability and recovery](docs/architecture/durability-recovery.md) ·
[platform evidence matrix](docs/architecture/platform-durability-evidence.md).

**C ABI.** The installed shared-library boundary is C ABI v1 (`GlifiStore::abi`,
`pkg-config glifistore-abi`), deliberately smaller than the C++ API, which stays unstable before 1.0.
See the [C ABI specification](docs/spec/c-abi-v1.md) and [ABI guide](docs/abi/README.md). An ABI
contract does not raise the product above architectural-prototype status.

**Large values (opt-in, embedded only).** A new durable paired Store may set
`StoreConfig::enable_blobs = true`: single-item `put`/`get`/`erase` then accept values up to
**64 MiB**, with `begin_blob_put` / `open_blob` for bounded streaming. Blob files are immutable and
typed references use an explicit persistence v2 profile; existing v1 Stores are not upgraded.
Daemon, SDK and C ABI support are deferred. See
[Large Blob Storage v1](docs/spec/large-blob-storage-v1.md) and its open gate.

## Clients

Every client implements default FNV wire-v2 routing, monotonic deadlines, bounded ordered pipelines
and the portable `committed` / `rejected` / `indeterminate` mutation outcome. A pipeline belongs to
one Worker; batch APIs group by Worker and restore caller order. **A batch is not a transaction.**

| Language | Package | Concurrency |
| --- | --- | --- |
| [C++](docs/reference/cpp-client-api.md) | Installable `GlifiStore::client` | Thread-safe Worker connections; pipeline and batch |
| [Python](sdk/python/README.md) | Standard-library sync and `asyncio` | Thread-safe sync client; native async client |
| [Perl](sdk/perl/README.md) | Synchronous, binary-safe | One client per process/thread; per-Worker pipelines in one select loop |
| [Go](sdk/go/README.md) | Synchronous native module | Mutex per Worker; batch fan-out with goroutines |
| [Erlang](sdk/erlang/README.md) | OTP application | Shareable coordinator; one `gen_server` per Worker |
| [Ruby](sdk/ruby/README.md) | Synchronous and optional `async` | MRI-thread-safe sync client; Fiber-aware async client |

Keyed SipHash-2-4 routing ([ADR 0030](docs/adr/0030-keyed-worker-routing.md)) is selected by the
extended `INIT` identity and implemented by the daemon and every official SDK; plain `GlifiStore/2`
keeps FNV-1a. Packages live in this tree and are not yet promised in every language registry.

```bash
./scripts/test-sdk-interop.sh
./scripts/test-secure-profile-interop.sh   # mTLS, authz, keyed routing, prefix, CRL, quotas
```

The secure-profile smoke runs in CI for C++, Python and Go, plus Perl, Ruby and Erlang when those
toolchains are present. Normative behavior: [client semantics v1](docs/spec/client-semantics-v1.md);
packaging state: [SDK roadmap](docs/architecture/sdk-roadmap.md).

## Evidence

```bash
./scripts/dev.sh test
./scripts/dev.sh asan
./scripts/dev.sh tsan
./scripts/dev.sh benchmark
./scripts/dev.sh benchmark-server --workers 4 --clients 4 --pipeline 32 \
  --executor-affinity --warmup 1 --repeats 5
./scripts/benchmark_sdk_clients.sh
python3 engineering/tools/validate_assurance.py
```

Benchmarks validate every response and report throughput; selected harnesses add latency, traffic
and memory. Hosted-runner results are regression signals, not product claims: a publishable
comparison needs identical code, workload, routing, durability, dataset, hardware, affinity and
measurement rules, as defined by the [benchmark standard](docs/spec/benchmark-standard.md). Raw
local output is gitignored.

Each promise is the conjunction of a requirement, a specification, an implementation, automated
proofs, retained evidence and an explicit residual risk — see the
[engineering baseline](docs/assurance/engineering-baseline.md).

## Documentation

Start at the [documentation map](docs/README.md). The browsable C++ reference is generated with
Doxygen ([build and publish](docs/development/documentation.md)); installed builds ship mdoc manuals
(`man 8 glifistored`, `man 7 glifistore`, section-1 tools — [man/README.md](man/README.md)).

| To… | Read |
| --- | --- |
| understand the system | [Architecture specification](docs/spec/architecture.md) · [code tour](docs/development/code-tour.md) · [glossary](docs/glossary.md) |
| embed the Store | [C++ API reference](docs/reference/cpp-api.md) · [C ABI v1](docs/spec/c-abi-v1.md) |
| build or use a client | [Wire protocol v2](docs/spec/wire-protocol-v2.md) · [client semantics v1](docs/spec/client-semantics-v1.md) |
| operate the daemon | [Operations handbook](docs/operations/handbook.md) · [CLI reference](docs/cli.md) |
| evaluate durability | [Persistence v1](docs/spec/persistence-v1.md) · [evidence matrix](docs/architecture/platform-durability-evidence.md) |
| review security | [Secure profile](docs/security/secure-profile.md) · [threat model](docs/security/threat-model.md) |
| trace requirements and gates | [Assurance catalog](engineering/README.md) · [quality gates](docs/production-readiness.md) |
| contribute | [CONTRIBUTING.md](CONTRIBUTING.md) · [code style](docs/development/code-style.md) · [test strategy](docs/development/test-strategy.md) |

## Platforms

- **macOS on Apple Silicon** is the primary development platform; macOS Intel remains a target.
- **Linux** x86-64 and Arm64, **FreeBSD** and **OpenBSD**/LibreSSL have build and test workflows.
  BSD VM rows and hosted runners are portability and regression evidence, not durability
  certification.
- No platform/filesystem row is E3/E4 power-loss certified.

## Security

Cleartext loopback is the default development posture. The daemon's secure profile requires
TLS 1.3, mTLS and a default-deny authorization map; every official SDK implements TLS/mTLS and the
extended `INIT` identity, subject to its language runtime's TLS support. This does not certify
hostile public or multi-tenant deployment. Report vulnerabilities as described in
[SECURITY.md](SECURITY.md); manage certificates and revocation with the
[secure-profile runbook](docs/operations/secure-profile-certs.md).

## Identity

*Glifo* — a mark carved into a surface; *store* — durable, addressable memory. The mark is a `G` of
three immutable Segment plates around one copper Record: **a mark that persists**. Sources, palette
and usage rules are in [artwork/](artwork/README.md).

## License

BSD-3-Clause — [LICENSE](LICENSE), [NOTICE](NOTICE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [licensing policy](docs/legal/licensing.md).
