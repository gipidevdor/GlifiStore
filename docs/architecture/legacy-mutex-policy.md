# Legacy mutex policy

**Normative** · embedded `Store::open` concurrency modes, 0.1.x · reviewed 2026-08-01

Requirements: `GS-CONCUR-PAIR-001`, `GS-CONCUR-LEGACY-001`. Authority:
[ADR 0032](../adr/0032-paired-concurrency-embedded-store.md).

The rules for `StoreConcurrencyMode::legacy_mutex`, a temporary compatibility escape hatch, removed
in 0.2, for embedded Store callers that cannot yet adopt the default paired Reader–Writer runtime
([ADR 0031](../adr/paired-reader-writer-shards.md), ADR 0032). It is not a second product runtime
and must not grow feature surface.

## Lifetime

| Line | Rule |
| --- | --- |
| 0.1.x | Deprecated but supported for embedded `Store::open` when explicitly selected. |
| 0.2 | **Removed.** Opening with `legacy_mutex` must fail closed with a clear error. |
| Daemon (`glifistored`) | Paired-only for the entire 0.1.x line; never offers `legacy_mutex`. |

The experimental `src/experimental/paired_*` code stays lab and microbenchmark only and is not a
selectable product runtime (ADR 0032).

## Feature freeze

While `legacy_mutex` exists, maintainers must not:

- add public API, storage or wire behavior that works only under `legacy_mutex`;
- document performance commitments unique to the mutex path;
- extend white-box tests that couple new product features to Worker mutex internals, except for
  security, correctness or recovery fixes that also apply to paired mode.

Work allowed on the legacy path:

- correctness, security and recovery fixes, with paired equivalence where applicable;
- test and CI support for the equivalence matrix below;
- deprecation warnings and migration guidance.

## Deprecation

- API: `StoreConcurrencyMode::legacy_mutex` stays marked deprecated in
  [`config.hpp`](../../include/glifistore/store/config.hpp) and the public documentation.
- Documentation: the glossary, the [public C++ API](../reference/cpp-api.md), the worker model and
  this policy state the removal in 0.2.
- Release notes: every 0.1.x release that still ships the hatch must mention the intended 0.2
  removal.
- Mixing: configuration is immutable at open, and public callers select exactly one enum value. No
  supported operation changes the mode of a live Store; any detected internal ownership violation
  fails closed (ADR 0032).

## Equivalence

Paired (the default) and `legacy_mutex` must agree on public outcomes for the same inputs, with
identical `StoreConfig` fields other than `concurrency`:

| Class | Required agreement |
| --- | --- |
| Sequential volatile | `put` / overwrite / `get` / `erase` / TTL expiry visibility |
| Concurrent distinct keys | Final per-key values after concurrent puts |
| Concurrent same key | Last writer wins; index verify succeeds |
| Close | Open → traffic → `close` drain completes without lost acknowledged volatile puts |
| Durable reopen (selected) | Persistence v1 / wire v2 bytes and reopen visibility unchanged across modes |

Out of scope for equivalence, as paired-only or legacy-only internals:

- `ReadGeneration` leases, Writer queue metrics, bounded lane depth;
- daemon asynchronous pair queues (the daemon has no legacy mode);
- crash and TSan harnesses that intentionally pin `legacy_mutex` to keep startup noise bounded;
- white-box Worker mutable-state inspection tests.

Proof: `tests/integration/runtime_equivalence_tests.cpp` (`GS-CONCUR-LEGACY-001`).

## Migration

Callers on `legacy_mutex` should:

1. open with the default `StoreConcurrencyMode::paired`, or omit the field;
2. keep owning `get` / `OwnedValue` usage ([ADR 0009](../adr/0009-public-read-ownership.md) is
   unchanged);
3. tolerate Writer-queue latency under multi-thread `put`/`erase` bursts;
4. drop any dependency on Worker-mutex fairness.

## Residual risk

Until the 0.2 removal, the cost of maintaining two paths remains. Equivalence tests cover public
outcomes, not a full linearizability formalization. No production-readiness claim attaches to either
path while quality gates remain below `ACCETTATA_PER_RILASCIO`.
