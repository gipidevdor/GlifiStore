# ADR 0036 slot-token local evidence — 2026-08-27

Status: local prototype campaign; not CI proof and not release evidence.

Scope: `src/experimental/paired_shard.cpp` and the test-only pool/storage headers under
`src/experimental/`. Neither implementation is selectable by `glifistored`. A later dormant core
bridge constructs the private production generation in candidate storage; its access header is not
installed and the official paired runtime remains unchanged.

Change exercised:

- single 64-bit release/acquire publication token `{epoch:48, slot+1:16}`;
- epoch validation before Reader adoption;
- checked 48-bit incarnation overflow;
- fixed-pool slot reuse telemetry;
- forced slot-reincarnation litmus checking epoch/visibility/value coherence.

Local host: Apple arm64, macOS, Apple Clang toolchain. Commands used the existing
`build/macos-release`, `build/macos-asan`, and `build/macos-tsan` configurations and selected the
test-name filter `ADR 0036`.

Results:

| Configuration | Focused tests | Result |
| --- | ---: | --- |
| Release | 8 | pass |
| ASan + UBSan | 8 | pass |
| TSan | 8 | pass |

The wider `paired` filter also passed 46/46 in the Release configuration and 57/57 in both
ASan+UBSan and TSan. The complete Release CTest run passed 50/52 in parallel; the persistence
fixture crossed its 15-second budget and the periodic crash checkpoint failed while the full crash
and allocation campaigns were running concurrently. Both were repeated serially and passed (4.23
seconds and 0.74 seconds respectively). This local scheduling-sensitive rerun is recorded rather
than misreported as a fully green parallel matrix.

The diagnostic prototype A/B and exact median rows are retained separately under
`benchmarks/results/adr0036-slot-token-2026-08-27/`. They are not production V11/V12 evidence.

The focused set covers V1 coherent adoption and actual slot reuse, V2/V3 pin and quiescence
reclamation, V6 fail-closed publication rejection, V7 merge under slot pressure, V9 bounded pool
exhaustion/recovery, and V13 stress.

## V8 production-congruent candidate extension

The same local Release, ASan+UBSan and TSan configurations passed five focused V8 candidate tests
plus one 20,000-publication concurrent stress test. Together with the earlier prototype cases, the
complete `ADR 0036` filter was 14/14 before the V6 extension. After the candidate, official V5,
fixed-shell, inline-owner and direct-object extensions it is 31/31 in all three configurations. The
pool and shell bank are compiled only into tests and store the real `PairReadGeneration` graph:

- fixed-pool exhaustion and recovery after Reader frontier advancement;
- exact minimum borrowed epoch held across successor publication;
- fail-closed rejection of a late regressing borrow frontier;
- durable compaction refresh while a cold read still borrows the previous Segment generation;
- Writer-owned durable rotation published as one new base generation;
- concurrent publish/adopt/reclaim stress without a TSan report.

The candidate protocol is described in
`docs/architecture/generation-slot-pool-candidate.md`. This is V8 candidate evidence only; no
official runtime behavior changed.

## V6 reservation/fail-closed candidate extension

Release, ASan+UBSan and TSan each pass two focused V6 candidate tests:

- full-pool reservation rejection occurs before Store mutation and does not invoke fail-closed;
- pre-linearization cancellation returns the `building` slot to the fixed pool;
- post-linearization generation failure invokes the fail-closed hook exactly once;
- a subsequent valid reservation proves the pool is not wedged;
- a real durable PUT is committed, deliberately left unpublished, marks the Store fail-closed, and
  is recovered into the candidate generation through `snapshot_durable_reads(..., true)`.

No ACK path or official runtime was modified. This evidence does not close production V6 or V9.

## V5 deterministic-shutdown candidate extension

Release, ASan+UBSan and TSan each pass four focused V5 candidate tests. The later complete 31-test
ADR 0036 filter is green in all three configurations:

- the Writer linearizes `stop_admission` after the Reader stops feeding the mutation lane;
- reservations already admitted remain drainable, while late reservations are rejected before
  Store mutation;
- Reader quiescence is refused while any `building` reservation exists;
- pre-linearization abandonment cancels safely and post-linearization abandonment invokes the
  fail-closed hook exactly once;
- a slow epoch borrow retains its retired graph until terminal Reader quiescence;
- a real durable `PairReadGeneration` is retained across shutdown and reclaimed only after Reader
  drain, leaving exactly the final published generation owned by the pool;
- post-quiescence adoption is rejected.

The candidate deliberately does not simulate the external SPSC mutation/completion queues or
socket output ownership. Their ordered drain must be proven when the protocol is integrated into
`ShardPairRuntime`. This is candidate V5 evidence, not closure of the production gate.

## V5 official-runtime shutdown baseline

The production Alternative A runtime now exposes a separate, idempotent Reader finalization after
mutation drain. `Store::close()` owns the transition after Store admission drain and maintenance
join; the daemon enters close only after Reactor join, Writer stop/join and disk-read executor
stop/join:

- publication is release-stored to null before retired ownership is cleared;
- the Reader safe frontier advances past the final Writer epoch;
- the final Writer generation and all retired generations are released after publication revocation;
- a counted `ReadLease` rejects the complete multi-lane transition before any lane is finalized;
- concurrent and repeated finalization is serialized off hot path and succeeds without changing
  counters or ownership;
- post-finalization adoption returns null;
- public `Store::close()` owns and proves the terminal transition after operation/maintenance drain;
- `reader_shutdown_finalized` and `shutdown_generations_reclaimed` expose the terminal state.

The dedicated production V5 test and the existing in-flight pinned cold-read server shutdown test
pass in Release, ASan+UBSan and TSan. The wider paired filter passes 46/46 in Release and 57/57 in
both sanitizer configurations. This is a local production-baseline result for the existing
shared-ownership runtime, not CI proof and not slot-pool production integration.

## V9 exact-capacity candidate extension

The candidate capacity is now a compile-time function of the official runtime retire bound:
`GenerationSlotCapacity<ShardPairRuntime::kMaximumRetiredReadGenerations>` equals 65 slots. One slot
is current and at most 64 are retired. At retire debt 63, the last free slot can be reserved before
Store entry; after publication it becomes current while the previous current becomes retired, so
the total remains 65. A `+2` formula was evaluated and rejected as unnecessary memory overhead.

The V9 test pins the initial Reader frontier, fills exactly all 65 slots, verifies that the next
reservation fails before Store mutation without overwriting publication, then advances the Reader,
reclaims to one live slot and successfully publishes again. Release, ASan+UBSan and TSan are green.
This proves the candidate formula and bounded recovery, not its integration into production
admission/backpressure.

Residual limits:

- this is local candidate evidence, not a production-integrated slot pool;
- the durable refresh, crash and production merge paths are absent from the prototype;
- no V11/V12 performance A/B has been recorded for a production-congruent implementation;
- the multi-OS sanitizer/CI matrix remains open.

Therefore ADR 0036 remains `proposed` and GlifiStore remains an architectural prototype.
