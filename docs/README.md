# Documentation

Every page that specifies, explains or records GlifiStore, listed once. Start from the task in front
of you; the sections below hold the rest.

| You want to | Start with | Then |
| --- | --- | --- |
| Understand the system | [Architecture specification](spec/architecture.md) | [Code tour](development/code-tour.md) · [Glossary](glossary.md) |
| Embed the engine | [Public C++ API](reference/cpp-api.md) | [C ABI](abi/README.md) · [Durability and recovery](architecture/durability-recovery.md) |
| Write a client | [Wire protocol v2](spec/wire-protocol-v2.md) | [Client semantics v1](spec/client-semantics-v1.md) · [Client conformance](spec/tcp-client-conformance-v1.md) |
| Operate the daemon | [Operations](operations/README.md) | [CLI reference](cli.md) · [Secure profile](security/secure-profile.md) |
| Judge readiness | [Quality gates](production-readiness.md) | [Assurance catalog](../engineering/README.md) · [Platform evidence](architecture/platform-durability-evidence.md) |
| Contribute | [Development](development/README.md) | [Code style](development/code-style.md) · [Test strategy](development/test-strategy.md) · [Decisions](adr/README.md) |

## Authority

When two pages disagree, the higher one in this list wins:

1. a versioned specification in `spec/`, for the version it names;
2. an accepted decision record in `adr/`;
3. a reference contract in `reference/`;
4. an architecture or development guide;
5. the repository README;
6. a roadmap, changelog entry, benchmark record or other historical note.

Generated views ([quality gates](production-readiness.md), [gate list](assurance/gates.md),
[hazard register](assurance/hazards.md), [package status](distribution/package-status.md)) are not
normative; their authority is [`engineering/`](../engineering/README.md). Other assurance pages
state their own status.

Code and tests are evidence that an implementation follows a contract; they are no substitute for a
missing contract. When code and a normative specification disagree, do not silently edit either
side: record the discrepancy, decide which behavior is intended, update or supersede the relevant
ADR, and add compatibility evidence before changing a persisted or wire-visible rule.

Architecture guides describe the current implementation and must label planned behavior
explicitly. A roadmap may track unfinished work but cannot redefine a format, an acknowledgement
point, an ownership rule or a compatibility guarantee.

Each guide states its status (normative, descriptive, roadmap or historical) and scope under the
title, with the date of its last review when one is recorded. How pages are written, reviewed and
built: [documentation](development/documentation.md).

## Specifications

Normative contracts, except [mutation lifecycle](spec/mutation-lifecycle.md) and
[connection drain](spec/connection-drain-state-machine.md), which describe the implementation as it
is. "Versioned" names the contract domain, not product maturity: GlifiStore remains an
architectural prototype.

| Page | Settles |
| --- | --- |
| [Architecture specification](spec/architecture.md) | Components, layering, runtime selection, ownership planes |
| [Concurrency and memory model](spec/concurrency-memory-model.md) | Threads, locks, atomics, linearization, shutdown |
| [Mutation lifecycle](spec/mutation-lifecycle.md) | The paired Writer's mutation path, as implemented |
| [Connection drain state machine](spec/connection-drain-state-machine.md) | Reactor connection drain and close, as implemented |
| [Persistence v1](spec/persistence-v1.md) | The durable namespace, binary formats, checksums and authority |
| [Recovery state-transition matrix v1](spec/recovery-state-matrix-v1.md) | The restart outcome of every interrupted transition |
| [Backup and restore v1](spec/backup-restore-v1.md) | The snapshot boundary; offline and online fenced copy |
| [SwissTableIndex v1](spec/index-v1.md) | The exact-key Index algorithm and its invariants |
| [Wire protocol v2](spec/wire-protocol-v2.md) | Framing and session semantics |
| [Client semantics v1](spec/client-semantics-v1.md) | Errors, retry, timeouts and late responses across clients |
| [TCP client conformance v1](spec/tcp-client-conformance-v1.md) | The official SDK checklist, bootstrap and retry pseudocode, fixtures |
| [Error taxonomy v1](spec/error-taxonomy-v1.md) | Status-to-category vectors across languages |
| [C ABI v1](spec/c-abi-v1.md) | The language-neutral ABI: layouts, ownership, mutation outcomes |
| [Large blob storage v1](spec/large-blob-storage-v1.md) | Opt-in values up to 64 MiB and the persistence v2 profile; [gates](../engineering/gates/large-blobs.yaml) open |
| [Benchmark standard](spec/benchmark-standard.md) | Workloads and what one reported result means |

## References

| Page | Settles |
| --- | --- |
| [Public C++ API](reference/cpp-api.md) | The embedded Store: operations, limits, errors, lifetimes |
| [C++ client](reference/cpp-client-api.md) | Routing, mutation outcomes and lifecycle of the TCP client |
| [C ABI](abi/README.md) | Versioning, lifetime, threading, errors and compatibility of the C ABI |
| [CLI](cli.md) | Every program, flag, profile, signal and exit code |
| [Glossary](glossary.md) | The terms these pages use |

Installed manuals (mdoc): [man/](../man/README.md).

## Architecture

How the implementation meets the contracts.

### Storage and durability

| Page | Settles |
| --- | --- |
| [Storage model](architecture/storage-model.md) | Keys, Records, Segments, the Index and vacuum |
| [Durability and recovery](architecture/durability-recovery.md) | Commit, acknowledgement and recovery ordering |
| [Durable runtime catalog](architecture/durable-runtime-catalog.md) | The durable backend and its mutation state machine |
| [Recovery implementation](architecture/recovery-implementation.md) | The recovery orchestrator |
| [Durable compaction](architecture/durable-compaction.md) | Crash-safe compaction invariants |
| [Durable compaction model](architecture/durable-compaction-model.md) | Definitions, theorems and proofs of oldest-prefix compaction |
| [Segment format](architecture/segment-format.md) | Segment header and Record fields |
| [Manifest format](architecture/manifest-format.md) | Manifest header and entries |
| [Segment files](architecture/segment-filesystem.md) | Segment file names, creation and lifecycle |
| [Persistence filesystem](architecture/persistence-filesystem.md) | Filesystem primitives behind every durable policy |
| [Namespace policy](architecture/namespace-policy.md) | The read-only data-directory audit |
| [Residency model](architecture/residency-model.md) | What lives in memory and what is read from files |
| [Lean memory algorithm](architecture/lean-memory-algorithm.md) | The memory design of ADRs 0044, 0045 and 0046 |
| [Platform durability evidence](architecture/platform-durability-evidence.md) | E0–E4 evidence levels and platform rows |

### Runtime and concurrency

| Page | Settles |
| --- | --- |
| [Architecture charter](architecture/architecture-charter.md) | Fixed decisions, safety rules, non-goals |
| [Server model](architecture/server-model.md) | Shard pairs, lanes and connection ownership in `glifistored` |
| [Worker model](architecture/worker-model.md) | Worker sizing from platform topology |
| [Maintenance controller](architecture/maintenance-controller.md) | Budgeted background maintenance |
| [Write-pressure coordinator](architecture/paired-write-pressure-coordinator.md) | Measured write-path costs and the coordinator boundary |
| [Legacy mutex policy](architecture/legacy-mutex-policy.md) | The deprecated embedded mutex mode |
| [Volatile prototype](architecture/paired-shard-volatile-prototype.md) | The lab-only TCP engine in `src/experimental/` |
| [Generation slot pool](architecture/generation-slot-pool-candidate.md) | The experimental candidate behind ADR 0036 |
| [Build hardening](architecture/build-hardening.md) | Compiler, linker and artifact requirements |

### Compatibility

| Page | Settles |
| --- | --- |
| [Version lifecycle](architecture/version-lifecycle.md) | Upgrade, downgrade and ABI promises before 1.0 |
| [N↔N-1 compatibility](architecture/n-n1-compatibility.md) | What one release promises the previous one |
| [Format compatibility](architecture/format-compatibility.md) | The persistence and wire codecs the 0.1.x line reads and writes |
| [Store migration](architecture/store-migration.md) | Offline Worker resharding |

### Performance and clients

| Page | Settles |
| --- | --- |
| [Where performance matters](architecture/where-performance-matters.md) | When engine speed changes application speed |
| [Hot-path performance](architecture/hot-path-performance-2026-08-01.md) | The 2026-08-01 lab program |
| [Rejected optimizations](architecture/hot-path-rejected-optimizations.md) | What was measured and dropped |
| [SDK roadmap](architecture/sdk-roadmap.md) | Shared client work before more languages |
| [Ruby SDK roadmap](architecture/ruby-sdk-roadmap.md) | The Ruby client's status |
| [SDK packaging](architecture/sdk-packaging.md) | The packaging checklist every SDK follows |

## Operations

| Page | Settles |
| --- | --- |
| [Operations](operations/README.md) | Runbook index, first start, restarts, incidents, signals |
| [Durable daemon](operations/durable-tcp-daemon.md) | A durable `glifistored` from profile to drain |
| [Drain and overload](operations/graceful-drain-and-overload.md) | Restarts, deploys and `OVERLOADED` |
| [Backup and restore](operations/backup-restore.md) | Copying, moving and recovering a Store |
| [Compatibility and migration](operations/compatibility-and-migration.md) | Upgrades, downgrades and Worker-count changes |
| [Corruption and repair](operations/corruption-repair.md) | Startup and verify failures, offline repair |
| [Observability](operations/observability.md) | Probes, `STATS`, logs and effective configuration |
| [Certificates](operations/secure-profile-certs.md) | TLS rotation and revocation |
| [Soak](operations/soak.md) | Soak profiles and their limits |
| [E3 campaign](operations/e3-campaign.md) | Preparing a pinned-row durability campaign |

## Security

| Page | Settles |
| --- | --- |
| [Secure profile](security/secure-profile.md) | TLS, mTLS and authorization of the daemon and SDKs |
| [Threat model](security/threat-model.md) | The security boundary of the engine and protocol |
| [Security roadmap](security/roadmap.md) | Open security work and its order |
| [TLS performance](security/tls-performance.md) | How to read the cost of TLS |
| [Actions pinning](security/actions-pinning.md) | How CI pins third-party actions |

Reporting a vulnerability: [SECURITY.md](../SECURITY.md).

## Development

| Page | Settles |
| --- | --- |
| [Development](development/README.md) | Building, testing, sanitizers, fuzzing and benchmarks |
| [macOS and Xcode](development/macos.md) | The primary development platform |
| [Code tour](development/code-tour.md) | Where each responsibility lives in the source |
| [Code style](development/code-style.md) | Rules for native code and how they are checked |
| [Test strategy](development/test-strategy.md) | Test tiers and what each one proves |
| [Documentation](development/documentation.md) | How these pages are written, reviewed and built |

Contribution rules: [CONTRIBUTING.md](../CONTRIBUTING.md) and [AGENTS.md](../AGENTS.md).

## Release and distribution

| Page | Settles |
| --- | --- |
| [Artifact delivery](distribution/artifact-delivery.md) | Build once, then candidate, verify, publish |
| [Release evidence](distribution/release-evidence.md) | Artifact-bound proof records and mandatory matrices |
| [Release checklist](assurance/release-checklist.md) | What a tagged release must carry |
| [Verify a release](distribution/verify-release.md) | Checksums, seals, manifest and provenance |
| [Package CI](distribution/package-ci.md) | Running and extending the packaging backends |
| [Package status](distribution/package-status.md) | Generated view of the package matrix |
| [BSD packaging](distribution/bsd-packaging.md) | FreeBSD and OpenBSD reference ports |
| [Release residuals](distribution/wave5-l7-residuals.md) | Open release, portability and supply-chain work |
| [Licensing](legal/licensing.md) | Copyright, contributions and redistribution |
| [TLS redistribution](legal/tls-redistribution.md) | Obligations when shipping TLS-enabled binaries |

## Assurance

| Page | Settles |
| --- | --- |
| [Quality gates](production-readiness.md) | Generated readiness view of every gate |
| [Gate list](assurance/gates.md) and [hazard register](assurance/hazards.md) | Generated gate and hazard tables |
| [Persistence v1 roadmap](v1-production-roadmap.md) | The backlog and acceptance criteria for persistence v1 |
| [Evidence taxonomy](assurance/evidence-taxonomy.md) | What counts as evidence, and how it is labelled |
| [Performance budgets](assurance/performance-budgets.md) | Regression, hardware and soak budget classes |
| [Memory-order inventory](assurance/memory-order-inventory.md) | Every atomic and its ordering |
| [ABA and generation audit](assurance/aba-generation-token-audit.md) | Reuse, generation and token hazards |
| [Failure atomicity audit](assurance/exception-failure-atomicity.md) | Exception safety at commit boundaries |
| [Branch protection](assurance/github-branch-protection.md) | Required repository settings and checks |
| [Engineering baseline](assurance/engineering-baseline.md) | The historical Phase A gap baseline |

The machine-readable authority behind these pages is the
[assurance catalog](../engineering/README.md).

## Decisions and records

| Page | Settles |
| --- | --- |
| [Decision records](adr/README.md) | Every architecture decision in force or open |
| [Benchmark records](benchmarks/README.md) | Dated measurements and the decisions they supported |
| [Evidence records](../engineering/evidence/README.md) | Retained campaign evidence |
| [Changelog](../CHANGELOG.md) | What changed in each version |

## Clients

| Client | Documentation |
| --- | --- |
| C++ | [C++ client](reference/cpp-client-api.md) |
| Python | [Python client](../sdk/python/README.md) |
| Perl | [Perl client](../sdk/perl/README.md) |
| Go | [Go client](../sdk/go/README.md) |
| Erlang/OTP | [Erlang client](../sdk/erlang/README.md) |
| Ruby | [Ruby client](../sdk/ruby/README.md) |

Every client follows [client semantics v1](spec/client-semantics-v1.md) and the
[conformance checklist](spec/tcp-client-conformance-v1.md).
