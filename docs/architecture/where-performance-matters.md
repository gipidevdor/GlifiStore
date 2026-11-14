# Where performance matters

**Descriptive** · product positioning and benchmark interpretation, not a wire or disk contract · reviewed 2026-08-01

When engine speed becomes application speed, when it does not, and how to read engine, raw-TCP and
SDK benchmark numbers. A faster key-value path does not accelerate every request equally: the gain
depends on how much of a request is spent on small, frequent lookups rather than business logic or
remote calls.

## Request shape

```text
Browser
  → reverse proxy
  → application
  → GlifiStore   (session, cache, flags, quotas, …)
```

One HTTP request may touch GlifiStore for authentication material, session state, cached profiles,
permissions, rate limits, configuration, tokens and similar data. When those operations are cheap:

- mean latency falls;
- the same hardware serves more concurrent users;
- fewer application servers are needed for the same load.

Capacity and latency under lookup-heavy load are the outcomes that matter to an operator, not a
single microbenchmark number.

## Lookup-heavy workloads

These paths issue many tiny operations, so engine and SDK costs compound across the site.

| Pattern | Why many small operations dominate |
| --- | --- |
| Distributed cache | Often 3–10 lookups per HTTP request |
| Session store | Every request: session → user → permissions → CSRF → expiry |
| API gateway | Token checks, rate limits, quotas, config |
| Chat / presence | Online?, last position?, typing?, last message? |
| Gaming | Leaderboards, inventory, matchmaking, presence |
| AI serving | Embedding/prompt cache, feature flags, session, metadata |

## Workloads where it changes little

```text
HTTP request
  → 20 ms business logic
  → 300 ms relational query
  → 100 ms external API
```

If GlifiStore spends 0.15 ms instead of 0.30 ms, the end-user latency is unchanged. Use GlifiStore
where the hot path is lookup-heavy; it does not hide slow dependencies.

## Product claim

The useful claim is conditional: the same application logic can handle more concurrent requests with
less CPU and fewer servers when the hot path is many small key-value operations.

Release and platform requirements beyond the embedded engine are tracked in
[production readiness](../production-readiness.md), the [v1 roadmap](../v1-production-roadmap.md)
and the [SDK roadmap](sdk-roadmap.md).

## Engine and SDK numbers

Engine, raw-TCP and public-SDK suites measure different work. The public SDK path includes API
validation, routing, request encoding, owned results and outcome construction; a raw-wire harness
may pre-encode frames. A slower SDK number therefore does not by itself cap server capacity.

Track client and engine regressions separately. Multi-language clients should not force continuous
core rewrites for framing costs.

Use `./scripts/benchmark_sdk_clients.sh` for the current comparison matrix and interpret it under
the [benchmark standard](../spec/benchmark-standard.md). Local `benchmark-results*` directories are
gitignored and must not be cited as durable repository documentation.

Same-machine macOS Apple Silicon measurements and cost maps for Store GET/PUT and TCP are recorded
in the [hot-path performance program](hot-path-performance-2026-08-01.md). Those figures are
architectural-prototype lab evidence, not production capacity claims. Daemon clients should keep one
connection per Worker (`BIND_WORKER`, owner-bound); embedded uniform multi-thread PUT still crosses
shard Writers and is not equivalent to that model.

## Perl SDK

The Perl client already aggregates a Worker pipeline, buffers reads and overlaps Worker sockets in
one `IO::Select` loop. Pipeline encoding appends validated header, key and value fragments directly
to the aggregate scalar; public request and result hashes and owned GET scalars remain. Those costs
are candidates for profiling, not presumed single-digit details.

| Priority | Action | Effect |
| --- | --- | --- |
| Use the API correctly | Pipelines + `execute_worker_pipelines` / `execute_batch` | Amortize syscalls and overlap independent Workers; select depth from throughput and tail latency |
| Production scale | One client per Hypnotoad/prefork worker process; no shared ithreads | Parallelism = N processes |
| Profile first | Allocation/copy counts, parser/buffer work, `IO::Select`, syscalls | Locate CPU and waiting time before choosing a technique |
| Pure Perl | Flatten internal metadata/results, avoid eager hashes/copies, reuse buffers where ownership permits | Reduce interpreter and allocator work without packaging cost |
| Optional native kernel | Narrow XS codec/routing path, only if the profile is dominated there | Trade packaging complexity for measured CPU reduction |
| Concurrency candidates | Event-loop adapter or `connections_per_worker`, after workload evidence | Improve application or hot-Worker concurrency; not automatically single-pipeline speed |

Work empirically: preserve semantics, change one cost centre, and rerun the same validated workload.
XS is one possible outcome of that process, not the starting assumption.

### Accepted passes

These are local advisory measurements, not release capacity claims.

| Change | Workload | Before | After |
| --- | --- | ---: | ---: |
| Lazy pipeline result slots | Perl 5.44, alternated same-process A/B, four Workers, pipeline 128 | 74.2–74.5 k ops/s | 75.9–77.3 k ops/s (about +2–4%) |
| Lazy pipeline result slots | pipeline-32 sample | 66.3 k ops/s | 71.4 k ops/s |
| `execute_batch` reuses grouping ownership | Perl 5.44, two same-server A/B pairs, four Workers, pipeline 128 | about 57.5–58.9 k ops/s | 72.2–74.7 k ops/s (about +25–27%) |
| Routing reuses the `INIT` identity, FNV | alternated same-server runs, four Workers, pipeline 128 | 74.0–74.5 k ops/s | 79.5–79.9 k ops/s (about +7–8%) |
| Routing reuses the `INIT` identity, keyed SipHash | check | 9.09 k ops/s | 9.19 k ops/s |
| Fragment assembly, 64-byte values | Perl 5.44, isolated A/B, pipeline 128 | 346.7 µs per encoded batch | 318.5 µs per encoded batch |
| Fragment assembly, 1 KiB values | Perl 5.44, isolated A/B, pipeline 128 | 352.5 µs per encoded batch | 323.9 µs per encoded batch |

- Lazy result slots: pipeline result slots stay unmaterialized until their actual success or failure
  is known. Previously every slot eagerly allocated a placeholder failure hash that the normal
  success path immediately discarded. The error paths already materialized every unresolved slot,
  so outcome ordering and mutation classification are unchanged. Pipeline 1 stayed flat in a
  shorter check.
- Batch routing: `execute_batch` no longer computes FNV-1a routing twice; it reuses the ownership it
  established while grouping. The public `execute_worker_pipelines` API still validates that every
  key belongs to its supplied Worker. The benchmark exposes a `--batch` mode, and the Perl matrix
  records it separately.
- `INIT` identity: the connected-client routing path reuses the normalized identity accepted during
  `INIT` instead of allocating and validating a new routing hash for every key. Public protocol
  helpers retain full validation, and dedicated client tests cover default FNV and keyed SipHash
  routing. The smaller SipHash gain confirms that pure-Perl SipHash rounds, not routing-state setup,
  dominate that profile. The benchmark connects before generating keys, so it exercises the routing
  identity the server actually advertises.
- Fragment assembly: about +9% in the encoding kernel. It removes a different cost than the rejected
  join-at-end candidate: the client keeps Perl's efficient incremental aggregate scalar but no
  longer builds a complete temporary frame with a copied key and value for every request. A current
  one-Worker end-to-end sample at pipeline 128 measured 86.3 k ops/s; without an alternating server
  A/B, that figure is a post-change observation, not an attributed throughput gain or release
  capacity claim.

### Rejected candidates

Five nearby pure-Perl candidates were rejected. Four were measured on the same workload:

- re-selecting avoidance after an outer readiness event, join-at-end frame assembly and an
  intermediate flat response decoder each regressed by roughly 2–4%. The client does ship a narrow
  readiness shortcut, which the [SDK roadmap](sdk-roadmap.md) records as retained: after the shared
  `IO::Select` loop reports a readable socket it skips only the first wait, and a partial response
  waits again under the absolute deadline. That record shows about +2.4% and +4.4% at depth 8 and a
  flat depth 128; it comes from separate local runs, so the two results are not reconciled;
- parallel request-id and frame-offset arrays in place of per-request metadata pairs (flattened
  metadata) varied from a small gain to a 4.3% regression when run order was reversed.

The fifth, an indexed `vec` loop in place of FNV's `unpack C*` byte list, measured about 452 k
versus 479 k hashes/s (roughly −6%) in an isolated Perl 5.44 run. Perl's existing scalar
copy-on-write, incremental concatenation, byte unpacking and small array representation were cheaper
or more stable than the alternatives in this profile.
