# SDK roadmap

**Roadmap** · native SDKs (C++, Python, Perl, Go, Erlang, Ruby) and the shared wire contract · reviewed 2026-09-15

The open work on the six native clients, the evidence each gap needs, and the rule that admits
another language. "Implemented" here describes source-tree behavior and tests; it does not mean that
every package has been published to its language registry or that the server is
production-certified.

The common contract is [wire protocol v2](../spec/wire-protocol-v2.md) plus
[client semantics v1](../spec/client-semantics-v1.md): binary-safe keys, owner-bound connections,
monotonic deadlines, bounded ordered pipelines, controlled reconnect, structured errors and
`committed` / `rejected` / `indeterminate` mutation outcomes.

## Current matrix

| Client | Public surface | Concurrency model | TLS 1.3 | Worker routing |
| --- | --- | --- | --- | --- |
| C++ | sync CRUD, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | thread-safe connection per Worker | yes (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |
| Python | sync + `asyncio`, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | mutexed sync / native async | yes (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |
| Perl | sync CRUD, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | one select loop across Workers; one client per process/thread | optional `IO::Socket::SSL` (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |
| Go | sync CRUD, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | mutex per Worker; goroutine batch fan-out | yes (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |
| Erlang | sync OTP API, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | shareable coordinator + per-Worker `gen_server` | yes (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |
| Ruby | sync + optional `async`, backup, HEALTH/READY/STATS, AF_UNIX dial, pipeline, batch, Worker vectors | MRI-thread-safe sync / Fiber async | yes (not with AF_UNIX) | FNV + keyed SipHash `INIT` extension |

All six use golden wire fixtures and take part in the default-routing interoperability harness.
Every official client decodes plain `GlifiStore/2` (FNV) and the extended SipHash `INIT` identity.
The cleartext, TLS and secure interop matrix also exercises `execute_worker_pipelines` (one
pre-sharded PUT→GET vector per Worker) against a live `glifistored`.

## Highest-priority gaps

### Secure-profile interoperability

`scripts/test-secure-profile-interop.sh` exercises mTLS, `--authz-map`, a pinned
`--worker-hash-seed` and prefix denial for every available SDK. CI `sdk-clients` requires all six
SDKs and verifies that out-of-scope and quota-refused mutations are classified as
`permission_denied` and `overloaded` respectively, both with `rejected` / `never` semantics, rather
than accepting any generic failure. Each quota assertion gets a fresh server-side budget. Under the
CRL scenario, every SDK must first connect with an allowed certificate and then fail with the
revoked one, so a broken client TLS configuration cannot satisfy the negative assertion.

Remaining: installed-artifact secure-profile evidence from a tagged release. CI `sdk-clients`
already runs the installed-artifact matrix described below, and `release.yml` runs it in its plain
profile, but no tagged release has retained that evidence yet. Documentation must not call the SDK
security train complete before one does.

The harness has a fail-closed `GLIFISTORE_INTEROP_USE_INSTALLED=1` mode: C++ and Go binaries inside
the checkout are refused, source load paths are not injected, and the Python, Perl, Ruby and Erlang
helpers verify that their loaded module is outside the repository. This is the isolation mechanism;
package orchestration supplies the artifact proof.

The package slice installs the built Python wheel, Perl tarball and Ruby gem, and extracts the Go
and Erlang tracked-source archives, into clean isolated prefixes, then reruns the secure-profile
matrix with source injection disabled. The C++ peer is rebuilt as an external consumer linked only
through the installed CMake package. The `sdk-clients` job is configured to run this after packaging
and the daemon build. On pushes to `main`, the same job retains a 30-day bundle with the complete
run log, commit and platform metadata, toolchain identities and package checksums: run evidence, not
a permanent release claim. Registry-shaped Erlang Hex publication and retained cross-version
artifact records remain open.

### Released-artifact compatibility

Source-tree interoperability is implemented. Still required:

- install every built package artifact and rerun conformance from the installed copy. The Python
  wheel and sdist, the Perl tarball, the Ruby gem and the C++ installed consumer are covered; Go's
  tracked tag snapshot has an external consumer proof; Erlang has an externally compiled
  tracked-source archive, while the registry-shaped Hex artifact remains open;
- test supported old-client/new-server combinations under the 0.x compatibility policy;
- publish checksums, provenance and SBOMs with tagged artifacts;
- record registry publication state rather than saying "install from registry" unconditionally.

Each SDK bundle carries a checksummed `sdk-release-index.json` with exact client roles,
distribution models and protocol and semantics versions, so a future N−1 artifact is selectable
without filename heuristics. The index deliberately records that compatibility evidence is not
inferred from catalog presence.

### Comparable performance evidence

`scripts/benchmark_sdk_clients.sh` is the comparison harness. A valid comparison fixes server
commit, routing mode, Worker and client counts, pipeline depth, value size, operation mix,
validation, TLS and durability mode, affinity, warmup and sample count. Report throughput together
with p50/p95/p99 where the harness exposes latency; do not infer a language limit from one pipeline
depth.

The harness reads the canonical root version, validates the exact expected matrix and labels a run
with an unavailable SDK as exploratory. Set `SDK_BENCH_REQUIRE_ALL=1` to produce a complete
cross-SDK comparison; it then fails before the expensive matrix starts if Ruby or Erlang is
unavailable. Ruby sequential and per-Worker threaded modes take part when a supported Ruby >= 3.2
runtime is available; an older or absent runtime is recorded as an explicit skip rather than
producing out-of-contract measurements.

Configurable connections per Worker remains measurement-gated for every SDK. It may improve
same-Worker concurrency but changes ordering, memory, reconnect and backpressure behavior.

For Perl, the next work is profile-led. Keep measuring scalar and hash allocation, frame copies,
buffer compaction, parser cost, `IO::Select`, syscalls and cross-Worker overlap before choosing an
implementation technique. Reduce pure-Perl allocation and copy costs first; evaluate a narrow XS
codec/routing kernel only if profiles show that boundary dominates. XS is not assumed to be the
only large lever. See the [Perl README](../../sdk/perl/README.md).

The client changes below were measured locally; they are development evidence, not retained release
evidence or a release gate.

C++: the public reference client has `glifistore_client_benchmark` with sequential, concurrent and
mixed-owner batch modes; the shared matrix runs the concurrent mode once per Worker/depth cell. The
benchmark is the retained improvement and uses the routing negotiated at `INIT`. Removing the
client's duplicate first-key pipeline hash was rejected: alternating local samples measured about
+0.8% at depth 1 and −0.4% at depth 8, and no stable throughput benefit justified the extra state in
the validation loop.

Python:

- Retained: sync and asyncio pipelines assemble validated header, key and value parts with one
  native final join, and leave response slots empty until the actual outcome is known. The second
  change halves the isolated result-materialization kernel at depth 128 (about 95.7 to
  47.3 microseconds) by removing 128 immediately discarded failure objects. Consecutive same-host
  one-Worker observations at depth 128 moved from 97.6k to 103.4k ops/s for sync and from 91.6k to
  96.0k for asyncio. These non-alternating samples support the removed-work decision but are not
  retained release capacity evidence.
- Retained: the buffered response decoder accepts an internal buffer offset directly, removing one
  temporary `memoryview` per response while preserving owned values and the public codec. Its
  isolated 256-frame corpus improved from about 455 to 326 microseconds (1.40×); consecutive
  post-change observations moved from 103.4k to 104.7k ops/s for sync and from 96.0k to 97.7k for
  asyncio, again as non-alternating development evidence only.

Perl:

- Retained: client routing reuses the normalized `INIT` identity, and the benchmark can generate
  both default-FNV and keyed-SipHash workloads.
- Retained: the shared Worker loop no longer performs a second readiness wait immediately after
  `IO::Select` dispatched a readable socket; partial reads still re-enter the absolute-deadline
  wait. Two local macOS-arm64 four-Worker passes at 200,000 PUT/GET pairs measured depth 8 at about
  +2.4% and +4.4%; depth 128 alternated between +1.3% and −0.9% and is treated as flat.
- Retained: a profile showed that every client path immediately discarded the public decoder's
  six-field hash. Client paths now use a compact internal tuple while the public named hash stays,
  which removes that transient allocation. Two alternating local passes measured +1.3%/+2.3% at
  depth 8 and +2.9%/+2.4% at depth 128.

Go (the benchmark is opt-in and requires a live daemon through `GLIFISTORE_BENCH_PORT`):

- Retained: pipeline ownership validation hashes the first key once rather than twice. A local
  macOS-arm64 loopback A/B at four Workers, pipeline depth 8 and 200,000 operations measured about
  225k versus 205k operations/s (~9.8%).
- Rejected: passing the batch's precomputed Worker into its internal pipeline. Depth 128 was flat
  (~312k operations/s both ways), while depth 8 regressed about 1.9%. The batch benchmark mode
  remains to keep this decision reproducible, and its workload uses the routing identity negotiated
  at `INIT`.
- Retained: Worker-indexed batch groups and disjoint positional result writes. End-to-end `benchmem`
  at four Workers and eight PUT/GET pairs per Worker reduced one batch call from 78 to 73
  allocations (−6.4%) and from about 26.25 KiB to 25.55 KiB (−2.7%), while throughput stayed flat
  within noise.
- Retained: lazily preallocated parallel request and index vectors remove the second request copy:
  73 to 61 allocations (−16.4%) and about 25.55 KiB to 16.40 KiB (−35.8%), again with flat
  throughput.
- Reverted: moving the two local batch closures into private helpers reduced the four-Worker case
  from 61 to 55 allocations but raised repeat median latency from about 306–308 µs to about 315 µs.
- Retained: a dedicated one-Worker fast path removes grouping and fan-out, reducing the 8-pair live
  benchmark from 16 to 10 allocations (−37.5%) and from 4,112 to 1,664 B/op (−59.5%). Host
  throughput samples were too noisy to claim a speedup; the acceptance is limited to removed work
  and stable allocation evidence, with positional admission-failure semantics covered by a unit
  test.

### Mutation idempotency (protocol candidate)

Clients classify uncertain mutations correctly, but applications cannot automatically retry an
`indeterminate` result. A future `client_id + operation_id` mechanism would require a protocol ADR,
bounded server retention and recovery semantics. It is not part of wire v2 today.

## Implemented shared foundation

- Keyed routing: [ADR 0030](../adr/0030-keyed-worker-routing.md) `INIT` decode and SipHash-2-4
  routing are in all six SDKs, with unit tests for paper vectors, plain and extended `INIT`, and FNV
  versus SipHash ownership divergence. Default interop starts with FNV, and the keyed daemon matrix
  (`--worker-hash-seed`) runs by default unless `INTEROP_KEYED=0`.
- Golden request and response fixtures are verified across the official SDKs.
- Official SDK unit fakes prove that standalone, pipeline, `execute_batch` and
  `execute_worker_pipelines` ERASE of an absent key maps wire `NOT_FOUND` to portable `rejected` /
  `not_found` / `new_attempt` (client semantics §3/§5), including the Python and Ruby async clients;
  pipeline, batch and worker-pipeline slots use outcome `failed`.
- `scripts/test-sdk-backup-interop.sh` exercises fenced online `BACKUP` against a durable
  `glifistored` for all six official SDKs (C++ through `glifistore_interop_client`).
- `scripts/test-sdk-interop.sh` covers default FNV routing with Workers 1/2/4/8, plus keyed SipHash
  cleartext for Workers 2/4 when `INTEROP_KEYED=1` (the default); binary keys, empty values, TTL,
  pipelines, `execute_worker_pipelines`, PUT→ERASE→structured `NOT_FOUND` (including one cross-SDK
  miss), missing-key `NOT_FOUND`, oversized local rejection and cross-client PUT→GET. TLS (FNV) is
  covered when its dependencies are available.
- `scripts/test-secure-profile-interop.sh` covers mTLS, authz, prefix, CRL, principal quotas and the
  keyed seed. Local runs may explicitly report unavailable optional toolchains; CI `sdk-clients`
  sets `SECURE_INTEROP_REQUIRE_ALL=1`, so cpp, python, go, perl, ruby and erlang cannot disappear
  from the matrix through a silent skip.
- Supply-chain CI packages the SDKs, writes `SHA256SUMS`, requires syft SPDX JSON, Cosign-signs
  tags and cross-checks archive digests across Linux builders
  (`.github/workflows/supply-chain.yml`).
- Pipeline APIs operate on one Worker and never retry automatically. Batch APIs group by Worker,
  overlap independent connections where supported and restore caller order; they are not
  transactions.
- Per-call deadlines reuse one absolute monotonic deadline across any permitted retry.
- Uncertain mutations are not mislabeled as rejected.
- Each SDK documents its actual thread, process, Fiber or OTP ownership model.
- Packaging verification scripts exist for every source package; registry publication is a separate
  release action.

## Admission rule for another language

A new SDK is not official until it has an isomorphism plan and passes:

1. wire fixtures, including both routing identities;
2. client-semantics error, timeout, retry and indeterminate-outcome tests;
3. default and keyed-routing interop;
4. cleartext, TLS and complete secure-profile interop;
5. pipeline/batch ordering and concurrency-contract tests;
6. install-from-artifact packaging verification;
7. the shared benchmark workload, with results labeled by runtime and execution model.

Related: [C++ client reference](../reference/cpp-client-api.md),
[SDK packaging](sdk-packaging.md), [where performance matters](where-performance-matters.md),
[production readiness](../production-readiness.md) and the
[Ruby SDK roadmap](ruby-sdk-roadmap.md).
