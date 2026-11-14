# Error taxonomy v1

**Normative** · wire v2, C++ client, official SDKs, embedded `ErrorCode` mapping · reviewed 2026-08-02

Requirement: `GS-PROTO-ERROR-001`

This specification is the **single normative authority** for the portable status → category →
retryability contract. It authorizes:

1. wire `ResponseStatus` integers in [wire protocol v2](wire-protocol-v2.md);
2. C++ `ErrorCode` values that map onto portable categories when exposed by the TCP client;
3. official SDK structured errors (exception class, `Error` type, or blessed map).

Behavioral narrative, automatic retry rules, deadlines, and pipeline positional outcomes remain in
[client semantics v1](client-semantics-v1.md) (ADR 0019). This document freezes the **cross-language
vector table** and the shared fixture contract. When texts diverge, prefer the stricter fail-closed
reading and open an ADR.

## 1. Portable categories (closed set)

| Category | Meaning |
| --- | --- |
| `invalid_argument` | Local validation failed, or wire `INVALID_REQUEST` / `UNSUPPORTED`. |
| `not_found` | Wire `NOT_FOUND`. |
| `overloaded` | Wire `OVERLOADED`, or local admission/resource limit before send. |
| `permission_denied` | Wire `PERMISSION_DENIED`. |
| `unavailable` | Client closed, routing metadata changed, or wire `NOT_BOUND` on a bound session. |
| `transport` | Socket I/O, connect failure after dial, peer EOF, or request deadline on the socket. |
| `protocol` | Framing/codec failure, mismatched `request_id`, `WRONG_OWNER`, non-empty mutation `OK`. |
| `internal` | Wire `INTERNAL_ERROR`, or unrecoverable client inconsistency. |

Language surfaces may use exceptions (Python), `Result`/`ErrorCode` (C++), or hashes/maps (Perl,
Erlang). The **category string** is the portable contract.

## 2. Wire status → category, mutation outcome, retryability

Golden vectors: [`tests/fixtures/error_taxonomy_v1.json`](../../tests/fixtures/error_taxonomy_v1.json).

| Wire status | Value | Category | Standalone `PUT`/`ERASE` outcome | Read retryability | Mutation retryability (bytes sent > 0) | Pipeline outcome | Unhealthy |
| --- | ---: | --- | --- | --- | --- | --- | --- |
| `OK` | 0 | (success) | `committed` only if value empty | — | — | `succeeded` | no |
| `INVALID_REQUEST` | 1 | `invalid_argument` | `rejected` | `never` | `reconcile_first` | `failed` | no |
| `UNSUPPORTED` | 2 | `invalid_argument` | `rejected` | `never` | `reconcile_first` | `failed` | no |
| `INTERNAL_ERROR` | 3 | `internal` | `indeterminate` | `new_attempt` | `reconcile_first` | `indeterminate` | no |
| `NOT_FOUND` | 4 | `not_found` | `rejected` | `new_attempt` | `new_attempt` | `failed` | no |
| `OVERLOADED` | 5 | `overloaded` | `rejected` | `never` | `never` | `failed` | no |
| `WRONG_OWNER` | 6 | `protocol` | `rejected` | `new_attempt` | `reconcile_first` | `failed` | **yes** |
| `NOT_BOUND` | 7 | `unavailable` | `rejected` | `never` | `never` | `failed` | **yes** |
| `PERMISSION_DENIED` | 8 | `permission_denied` | `rejected` | `never` | `never` | `failed` | no |

Notes:

- Mutation retryability in the table assumes a response was decoded after `bytes_sent > 0`.
- `OVERLOADED` collapses admission pressure and durable capacity exhaustion; clients must not infer
  which cause applied ([client semantics §3](client-semantics-v1.md)).
- Unknown numeric statuses (outside 0–8): response codecs reject after the full frame is buffered
  (stream stays synced). Defensive `from_status` / `error_from_wire_status` helpers map them to
  category `protocol`, read retryability `new_attempt`, mutation outcome `indeterminate`, and keep
  the original numeric `wire_status`. Shared fixture includes `unknown_wire_status` (99).

## 3. Embedded `ErrorCode` → portable category (C++ TCP client)

When the C++ TCP client maps a wire status or local failure into `glifistore::Error`, the portable
`Error::category` must be one of the §1 names. Normative collapses used by the official client:

| `ErrorCode` | Portable category |
| --- | --- |
| `invalid_argument`, `record_too_large` | `invalid_argument` |
| `not_found` | `not_found` |
| `resource_exhausted` | `overloaded` |
| `unavailable` | `unavailable` (embedded/API); Reactor wire map uses `INTERNAL_ERROR` because Store sticky/fail-closed paths may already have linearized — see §5 |
| `io_error` | `transport` |
| `corrupted_data` | `protocol` |
| `internal_error` (and unlisted defaults) | `internal` |
| wire `PERMISSION_DENIED` | `permission_denied` (category set explicitly) |

Store-embedded paths may leave `category` / `retryability` empty; TCP clients must populate them
when returning errors to applications ([client semantics §2.1](client-semantics-v1.md)).

## 4. Conformance

Official SDKs and the C++ client must pass the shared fixture matrix:

| Runtime | Test |
| --- | --- |
| C++ | `tests/unit/client_error_taxonomy_tests.cpp` |
| Python | `sdk/python/tests/test_error_taxonomy.py` |
| Go | `sdk/go/client/error_taxonomy_test.go` |
| Perl | `sdk/perl/t/03-error-taxonomy.t` |
| Ruby | `sdk/ruby/test/test_error_taxonomy.rb` |
| Erlang | `sdk/erlang/test/glifistore_error_taxonomy_SUITE.erl` |

Fixture copies under `sdk/*/…/fixtures/` (or Go `testdata/`) are synced from
`tests/fixtures/error_taxonomy_v1.json` via `scripts/sync-sdk-fixtures.sh`.

## 5. Reactor embedded `ErrorCode` → wire status

The daemon Reactor maps completion/`queue_response` errors through
`reactor_detail::response_status`:

| `ErrorCode` | Wire status | Rationale |
| --- | --- | --- |
| `resource_exhausted`, `storage_exhausted`, `file_too_large`, `descriptor_exhausted`, `read_only_filesystem`, `sequence_conflict`, `segment_full`, `segment_sealed`, `arithmetic_overflow` | `OVERLOADED` | Admission / capacity / conflict / segment fit / **pre-Store lane expiry** (including shutdown-drain abandon of still-queued work) — known not newly committed by this attempt (server may have retried `sequence_conflict` internally). |
| `unavailable` | `INTERNAL_ERROR` | Fail-closed and sticky post-commit paths may have linearized; must not claim known-not-committed. |
| `not_found` | `NOT_FOUND` | |
| `invalid_argument`, `record_too_large` | `INVALID_REQUEST` | |
| other / `internal_error` / `io_error` / `corrupted_data` | `INTERNAL_ERROR` | Integrity / I/O / fail-closed — reconcile when mutation bytes may have hit the wire path. |

Writer-normalized known-not-committed durable failures rewrite any Reactor
`INTERNAL_ERROR`-bucket code (`io_error`, `unavailable`, `corrupted_data`,
`internal_error`, `invalid_record`, `checksum_mismatch`, `invalid_reference`, …) to
`resource_exhausted` before completion, so they surface as wire `OVERLOADED` /
client `rejected` rather than `INTERNAL_ERROR` / `indeterminate`. Codes that already
map to rejected polarity (`segment_full`, capacity, `not_found`, `invalid_argument`,
…) are left alone. Volatile Writer applies the same rewrite to Store-mutate failures
that never crossed append (e.g. rotation `invalid_reference`); post-append Index
failures stay `unavailable` (sticky) and must not be demoted to `OVERLOADED`.
`mutate_durable_batch` converts pre-mutate allocation/unexpected failures into
per-slot `not_committed` + `resource_exhausted` (no Writer sticky) so a throw cannot
escape after the Writer has already set `durable_mutate_entered`.
Already-queued siblings rejected before Store entry after sticky
fail-closed also stamp `resource_exhausted` directly — including sync mid-chunk
abort of never-Store-entered items and sync incremental-merge backpressure (matching
async). Sync volatile catch after a Store mutate must not upgrade those never-entered
siblings to `unavailable` (keep known-not-committed polarity; only unpublished
Store-entered nodes stay `unavailable`). Sync/async durable_group batch catch follows
the same rule: preserve mid-chunk fail-closed / rewritten errors; upgrade the pre-mutate
placeholder to `unavailable` for the in-flight sub-batch **through result classification**
after `mutate_durable_batch` returns (write boundary may have been crossed — not only while
the mutate call is on the stack); later never-started sub-batch placeholders stay
`resource_exhausted`. Committed and indeterminate durable failures still become
`unavailable` → `INTERNAL_ERROR`.
New admissions after sticky still reject at `try_submit` (wire `OVERLOADED`) or at the
sync Store API with `unavailable` (not a linearized mutation). Already-queued async work
rejected before Store entry after sticky stamps `resource_exhausted` (wire `OVERLOADED`).

True admission rejects on the mutation path still set `ResponseStatus::overloaded` directly
(without going through `ErrorCode::unavailable`).

## 5.1 Internal Writer causes (wire mapping frozen)

Structural refactor may tag internal mutation causes for diagnostics. Wire polarity remains
centralized:

| Internal situation | Authoritative rewrite | Wire |
| --- | --- | --- |
| Known-not-committed (never linearized) | `rewrite_known_not_committed_wire_error` → keep reject codes or `resource_exhausted` | `OVERLOADED` (or NOT_FOUND / INVALID_REQUEST) |
| Post-commit / indeterminate / sticky | force `unavailable` before queue_response | `INTERNAL_ERROR` |
| Pre-Store expire / fail-closed never-entered | `resource_exhausted` | `OVERLOADED` |

Invariant rules:

- `OVERLOADED` ⇔ known-not-committed for mutation ACKs;
- post-commit visibility/publication failure → `unavailable` / wire `INTERNAL_ERROR`;
- never demote `unavailable` through `rewrite_known_not_committed_wire_error`.

Implementation: `mutation_execution.hpp` (`rewrite_known_not_committed_wire_error`,
`classify_volatile_mutation_error`) + `reactor_detail::response_status`.

## 6. Residual gaps

- Transport / deadline / local-validation vectors beyond the shared fixture remain covered by
  client-semantics narrative tests, not this fixture.
