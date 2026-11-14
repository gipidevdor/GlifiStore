## [Unreleased]

- Correct two rows of the recovery state matrix that disagreed with the implementation. Ordinary
  recovery without a compaction intent validates each recognized private temporary, including
  `.glifistore.compaction.tmp`, removes it and syncs the directory, as the durability recovery
  guide and the runtime catalog page already say and a characterization test pins; intent recovery
  leaves them for the next ordinary recovery. No behavior change. Claim ceiling unchanged.

- State strict-prefix durable compaction as a mathematical model
  ([durable compaction model](docs/architecture/durable-compaction-model.md)): definitions,
  theorems and proofs for prefix closure, foreground commutation, the `(k, m)` transition, the
  choice of prefix, output counts and recovery, with the assumptions they need. An executable
  reference model checks the theorems by exhaustion (all 442,276 histories of up to five Records
  over four sealed Segments with every prefix and Index, 115,240 selections, every namespace state
  of up to four sources and four outputs), and the runtime is compared with it on every history of
  up to three Records, under every foreground behavior and over 58 namespace states. No format,
  wire, acknowledgement or recovery change. Claim ceiling unchanged.

- Prove strict-prefix durable compaction (`k < n` oldest sealed Segments, ADR 0044), which every
  earlier fixture skipped by replacing the complete sealed set: canonical `(k, m)` transition and
  validator, planner, copy-budget selection and intent codec (unit); recovery under both
  authorities with partial retirement and rollback; a PUT/overwrite/ERASE/TTL model with tombstones
  and expired puts inside and after the prefix, selection, rebase and filesystem-fault enumeration
  (integration); and a 25-checkpoint SIGKILL scenario in `glifistore_crash_sync` (now 114
  checkpoints). Fix `CompactionResult::retained_sealed_segments`, which stayed at the pre-rebase
  value when a rotation of the same Worker during the build left a newer sealed Segment after the
  prefix. No format, wire, acknowledgement or recovery change. Claim ceiling unchanged.

- Report an exact status for every item when a `put_batch` / `erase_batch` allocation fails midway:
  items of owners that were already applied are no longer relabelled `resource_exhausted`, which
  means known not committed. The Store now sizes its batch buffers before the first submission, its
  commit phase never throws, and `put_batch` and `erase_batch` share one implementation. Add
  `allocation_fault_batch_tests`. No format, wire or acknowledgement change. Claim ceiling
  unchanged.

- Group `Store::put_batch` / `erase_batch` items by owner with a stable counting partition
  (`OwnerPartition`) instead of a comparison sort: `O(n + owners)` time and one allocation instead of
  `O(n log n)` comparisons and moves of 64-byte entries, with caller order inside every owner by
  construction. Add `owner_partition_tests`. No format, wire or acknowledgement change. Claim ceiling
  unchanged.

- Make `ShardPairRuntime::mutate_batch` throw only before it admits a batch: it now waits for every
  node and then moves the statuses out, instead of copying each status (which can allocate) while
  later nodes are still queued. No behavior change on a path that does not run out of memory. Claim
  ceiling unchanged.

- Keep the earlier unflushed siblings of a `durable_group` Writer batch when a later sibling is
  rejected before it touches the pending group (erase of an absent key, live-key budget, validation or
  admission limit). The durable runtime no longer fails closed for such a request, so a pipelined PUT
  keeps its `OK` instead of `INTERNAL_ERROR` and later mutations are served instead of `OVERLOADED`.
  Failures during or after the append attempt still abandon the prefix. ADR 0047, HAZ-044.
  Claim ceiling unchanged.

- Fix `Store::put_batch` / `erase_batch` applying one owner's items out of caller order on
  multi-owner batches: the owner grouping used an unstable sort keyed on the shard alone, so a batch
  beyond the standard library's small-range insertion sort could end with the wrong value for a
  repeated key or attach `not_found` to the wrong item. Grouping now orders by shard and input index,
  restoring the documented per-owner FIFO (C++ API, C ABI batches). No format, wire or
  acknowledgement change. Add `paired_store_batch_order_tests` (volatile, Writer-path and durable
  Stores, with reopen) and a four-Worker C ABI check. Claim ceiling unchanged.

- Rewrite the documentation for clarity and economy. Each guide opens with one status line
  (Normative, Descriptive, Roadmap or Historical);
  [documentation](docs/development/documentation.md) holds the writing and review standard and
  [docs/README.md](docs/README.md) maps every page.
  Overlapping pages now have one home each: the ABI pages, the index, vacuum and safety models, the
  public API contract, the architecture backup note, the operations handbook, Worker resharding and
  the documentation roadmap merge into their nearest guide or specification; the final engineering
  report and the debt-remediation lanes are retired. The documentation site build no longer fails
  on the shared `blob.hpp` file name or an `index` heading. No contract, format or behavior change.
  Claim ceiling unchanged.

- Retain ADR 0036 shell/publication diagnostics and interleaved official Store flag A/B evidence
  on Apple M4; recommend rejecting promotion after a 6.69% worker-affine PUT 2t regression,
  leaving ADR status and production defaults unchanged. Claim ceiling unchanged.

- Gate ADR 0036 candidate tests, bridge and generation benchmarks behind the default-OFF
  `GLIFISTORE_BUILD_LAB` option; isolate candidate integration tests and add one lab CI job plus
  a production Store A/B measurement driver. Claim ceiling unchanged.

- Remove the ADR 0032 volatile paired prototype, its tests and benchmark targets, and separate
  the remaining ADR 0036 test bridge without changing the official runtime. Claim ceiling
  unchanged.

- Fix clang-format drift that failed Static analysis on `main` (client/store/daemon erase_batch
  paths and related litmus). Claim ceiling unchanged.

- Move maintenance emergency mutation-gate / batch TOCTOU litmus into
  `maintenance_controller_emergency_gate_tests` so `maintenance_controller_pressure_tests` stays
  under the 1600-line structure gate. Claim ceiling unchanged.

- Move reactor connection/request/accept rate-limit and idle/request-timeout litmus into
  `server_reactor_limits_timeout_tests` so `server_reactor_security_tests` stays under the
  1600-line structure gate. Claim ceiling unchanged.

- Move blocked durable I/O mutex-hold litmus into
  `persistence_recovery_crash_mutex_hold_tests` so `persistence_recovery_crash_tests` stays under
  the 1600-line structure gate. Claim ceiling unchanged.

- Prove official SDK `execute_worker_pipelines` ERASE of an absent key maps wire `NOT_FOUND` to
  slot `failed` with `rejected`/`not_found`/`new_attempt` (client-semantics §3/§5; closes the
  erase-absent surface across standalone / pipeline / batch / worker-pipelines). Claim ceiling
  unchanged.

- Prove official SDK `execute_batch` ERASE of an absent key maps wire `NOT_FOUND` to slot
  `failed` with `rejected`/`not_found`/`new_attempt` (client-semantics §3/§5; parity with
  standalone and pipeline erase-absent); refresh client-semantics / sdk-roadmap / public API
  review stamps. Claim ceiling unchanged.

- Move exclusive-Writer compaction `hot_path_depth` / Worker-mutex litmus into
  `persistence_recovery_compaction_writer_hot_path_tests` so
  `persistence_recovery_compaction_tests` stays under the 1600-line structure gate. Claim ceiling
  unchanged.

- Prove official SDK pipeline ERASE of an absent key maps wire `NOT_FOUND` to pipeline slot
  `failed` with `rejected`/`not_found`/`new_attempt` (client-semantics §3; parity with standalone
  erase-absent). Claim ceiling unchanged.

- Move volatile/legacy `erase_batch` litmus into
  `paired_store_erase_batch_volatile_litmus_tests` so `paired_store_litmus_tests` stays under the
  1600-line structure gate. Claim ceiling unchanged.

- Prove async Python/Ruby standalone ERASE of an absent key maps wire `NOT_FOUND` to
  `rejected`/`not_found`/`new_attempt` (parity with sync SDK fakes). Claim ceiling unchanged.

- Prove live C++ online `BACKUP` in `test-sdk-backup-interop.sh` via `glifistore_interop_client
  backup --dest` (six official SDKs on the runtime smoke). Still fenced, not zero-fence. Claim
  ceiling unchanged.

- Prove official SDK standalone ERASE of an absent key maps wire `NOT_FOUND` to
  `rejected`/`not_found`/`new_attempt` (client-semantics §3); fix Ruby/Erlang fake servers that
  always answered OK for ERASE. Claim ceiling unchanged.

- Prove volatile C ABI `gs_store_erase_batch` same-key FIFO (`GS_NOT_FOUND` +
  `GS_MUTATION_INDETERMINATE`) and document that engine-mapped failures stay indeterminate while
  facade pre-engine rejects use `GS_MUTATION_REJECTED`. Claim ceiling unchanged.

- Prove `erase_batch` tombstones across reopen for embedded and Writer-path `durable_periodic`
  (shutdown flush, not timer), completing the public durable-mode matrix alongside sync/group.
  Claim ceiling unchanged.

- Prove `erase_batch` tombstones across reopen on Writer-path `durable_sync` (async lane) and
  `durable_group`, alongside the existing embedded durable_sync litmus. Claim ceiling unchanged.

- Prove live official-SDK wire ERASE in `test-sdk-interop.sh` (PUT → ERASE → structured
  NOT_FOUND, plus one cross-SDK miss) on the cleartext/TLS/secure matrices. Claim ceiling
  unchanged.

- Prove non-paired `Store::erase_batch` maintenance emergency entry reject and mid-batch TOCTOU
  parity with `put_batch`, and document `erase_batch` on the emergency mutation gate. Claim
  ceiling unchanged.

- Prove durable_sync `erase_batch` tombstones across reopen
  (`paired_store_erase_batch_durable_litmus_tests`, kept under the 1600-line structure
  gate) and C ABI durable `gs_store_erase_batch` with same-key FIFO `not_found` plus
  reopen survivor checks. Claim ceiling unchanged.

- Prove live multi-Worker `execute_worker_pipelines` across official SDKs in
  `test-sdk-interop.sh`, add C++ HEALTH/READY/STATS to live probe interop, make
  TLS-capability detection honest for TLS-off builds, retarget the flaky
  cmake.org `CheckLinkerFlag` doc link, and exclude cmake.org from Lychee to
  stop chronic 5xx/timeout Docs links failures. Claim ceiling unchanged.

- Refresh pinned `tla2tools.jar` SHA-256 after the upstream v1.8.0 GitHub release asset
  was re-published (Formal models workflow). Claim ceiling unchanged.

- Prove ADR 0037 Phase C reactor mutation windows and GET/PING drain barriers with
  `server_reactor_mutation_window_tests` (contiguous PUT window/resume, PUT→ERASE→GET,
  durable_sync barriers under `BlockingFileSync`); add `legacy_mutex` `erase_batch` litmus
  and async Python/Ruby `execute_worker_pipelines` coverage. Claim ceiling unchanged.

- Close official SDK `execute_worker_pipelines` parity (C++/Python/Go/Ruby sync+async) with
  Perl/Erlang: pre-sharded Worker vectors, ownership validation, shared deadline, empty-slot
  `[]`; align sdk-roadmap / client-semantics / C++ client docs; document `Store::erase_batch` in
  public C++ API references. Claim ceiling unchanged.

- Add `Store::erase_batch` (paired + non-paired) with litmus coverage, C ABI
  `gs_store_erase_batch` / `gs_erase_request` (ABI minor 1.1), daemon
  `--close-drain-deadline-ms`, and live SDK HEALTH/READY/STATS interop against
  glifistored; align FreeBSD/OpenBSD packaging ABI filenames with `ABI_VERSION`.
  Claim ceiling unchanged.

- Harden sealed N-1 package selection: token-bound SemVer matching (no `10.0.9`/`0.0.90`
  false positives), refuse mixed architectures without an explicit filter, verify
  `SHA256SUMS` when present; stop validating N-1 Linux installs against N rendered
  metadata; refuse `GLIFISTORE_N1_PACKAGE_DIR` when no SemVer predecessor exists;
  narrow MacPorts/Homebrew residuals so tip-rendered recipes vs sealed source bytes are
  honest; align Python/Perl/Ruby READMEs with HEALTH/READY/STATS. Claim ceiling unchanged.

- Correct Erlang/Go SDK README drift after the HEALTH/READY/STATS surface landed:
  Erlang no longer claims codec-only probes with no public RPC; Go layout lists
  Health/Ready/Stats and AF_UNIX (TLS mutually exclusive). Claim ceiling unchanged.

- Prove official language-SDK HEALTH/READY/STATS (and AF_UNIX⊕TLS refusal) against
  fake servers, and correct client comments that wrongly claimed empty HEALTH/READY
  payloads on OK (wire v2 returns `GlifiStore/live` / `GlifiStore/ready`). Align
  cpp-client-api, SDK roadmap, TCP client conformance, packaging debian/rpm residuals,
  and engineering README N-1 walk wording with the landed surface; wire
  `GS-CORE-CLOSE-001` → `GATE-CONCURRENCY-SPEC`. Claim ceiling unchanged.

- Client SDK surface closure: expose wire HEALTH/READY/STATS and AF_UNIX dial
  (mutually exclusive with TLS) on C++/Python/Go/Perl/Ruby/Erlang clients; add
  `routing()` getters where missing (C++/Perl/Erlang). Promote documentation,
  static-analysis, fail-closed I/O, install-consumer, C ABI, API/ABI policy, and
  concurrency-spec gates to `PROVATA_IN_CI` where CI already proves them; fix
  stale `rebuild_index` wording. Claim ceiling unchanged.

- Implement install→seed→upgrade→verify walks for FreeBSD/OpenBSD (sealed `.pkg`/`.tgz`)
  and MacPorts/Homebrew (sealed `GlifiStore-<version>.tar.xz` sources) when
  `GLIFISTORE_N1_PACKAGE_DIR` supplies predecessor bytes; extend N-1 artifact selection
  accordingly. Claim ceiling unchanged; no sealed N-1 PASS invented.

- Move the live RPM package-ci target from EOL Fedora 41 to digest-pinned Fedora 43;
  make the Go license gate an honest stdlib-only pass (no soft-OK after go-licenses fatal);
  require `go mod tidy -diff` under the Go 1.27 floor. Claim ceiling unchanged.

- Pin mise Ruby bootstrap to checksummed v2026.9.5 (remove `curl | sh`); annotate common
  Action pins to exact tags; read `VERSION` in per-SDK bench scripts; raise the C++ package
  verify fallback CMake floor to 3.25; correct TLC “best-effort” wording and formal checklist
  docs. Claim ceiling unchanged.

- Correct packaging admission residuals that wrongly claimed FreeBSD/OpenBSD lacked
  package-matrix `package-upgrade` rows; align OpenBSD package-producer residual with the
  existing same-run release path; refresh Action pin counts; annotate `openbsd-vm` as
  `# v1.4.7`; checksum-pin Go 1.27.1 bootstraps; bump Syft to v1.51.1; align Python SDK
  classifiers with CI 3.14 and Pre-Alpha claim ceiling. Bound the CMake
  `glifistore_benchmark_parallel_single-worker` smoke to 1×2 so generation-admission
  `resource_exhausted` on slow FreeBSD VMs is not misread as a hit-count visibility failure.
  Claim ceiling unchanged.

- Refresh hosted toolchains and Action pins across the project: Go 1.27.x (already),
  Python 3.14, Ruby 3.4, `github/codeql-action` 4.38.0, `actions/setup-java` 6.0.1, and
  `vmactions/freebsd-vm` 1.5.6 (SHA-pinned). Re-pin `tla2tools.jar` v1.8.0 digest after the
  upstream asset changed. Bootstrap Go 1.27.1 on OpenBSD LibreSSL CI when ports `go` lags
  the SDK floor (and pins `GOTOOLCHAIN=local`). Claim ceiling unchanged.

- Bump the Go SDK language version and all CI/packaging Go toolchains to 1.27.x
  (bootstrap pin 1.27.1, the current stable). Claim ceiling unchanged.

- Packaging documentation and assurance alignment: update `GS-RELEASE-PACKAGE-001`,
  `GS-RELEASE-UPGRADE-001`, `GATE-PACKAGE-LIFECYCLE`, `GATE-PACKAGE-ADMISSION`, and
  `HAZ-034` so residuals match retained nightly evidence (Linux `LIFECYCLE_VERIFIED`,
  cross-SDK PASS) and the implemented Linux install→seed→upgrade→verify walk under
  `GLIFISTORE_N1_PACKAGE_DIR`; regenerate package-status and production-readiness;
  align package-ci, wave5 residuals, BSD packaging, and MacPorts/Homebrew READMEs.
  Claim ceiling unchanged (architectural prototype); no gate promotion.

- CI quality follow-up: remove all 24 fail-closed unchecked-`optional` diagnostics across 10
  production sources, with checked local-reference extraction that remains data-flow-visible across the
  clang-tidy versions used locally and in CI; make both continuous-Reader paired litmus tests
  distinguish visibility and linearizability failures from the documented 64-generation
  backpressure bound, using one shared explicit Reader-quiescence handshake before retrying the
  same mutation; replace the Erlang fake server's racy read/modify/write held-client list with
  per-process ETS registrations and a release-before-collect handshake. No persistence, wire,
  ACK, routing, or public mutation-visibility contract changes.

- Quality closure: normalize all C++ sources to the pinned clang-format 21.1.8 gate; add a
  non-mutating `dev.sh verify` entry point; validate UTF-8 and exact-case repository-local links in
  every tracked Markdown document; repair the seven stale platform-durability evidence links; and
  wire the new documentation requirement/gate into assurance without promoting it beyond
  `IMPLEMENTATA` before CI evidence exists. Add a compile-database-driven, production-only
  fail-closed clang-tidy gate for unchecked optional access, use-after-move, analyzer dead stores,
  and declaration/definition parameter drift; harden the reported ownership, optional-state, and
  dead-store sites while retaining the wider all-target scan as an explicitly diagnostic signal.
  On macOS, exempt only the test-only global-allocation replacement harness from clang-tidy 21's
  incompatible libc++ `__builtin_operator_delete` analysis; it remains compiled and executed. Fix
  a TSan-confirmed dedicated-Writer race by deriving chunk failure state before releasing the
  caller's stack-backed completion node.

- ADR 0036 Wave 1 opt-in hardening: prove incremental merge + post-cut publication under
  generation-slot pressure in embedded and dedicated Writer paths. Terminal Reader shutdown now
  discards an unfinished Writer-only merge after mutation admission is stopped, Writers are joined,
  and counted Reader leases are absent; this releases the merge-cut slot pin before final reclaim.
  Focused Debug, Release, ASan+UBSan and TSan rows pass locally. ADR 0036 remains proposed;
  `generation_slot_pool` remains false by default and V8/V11/V12/multi-OS evidence stays open.

- Structure / ACK hygiene follow-up (architectural prototype unchanged): unify Writer
  ACK-after-visibility on DualPath `load_published_generation`; share
  `include/glifistore/core/little_endian.hpp`; split oversized production TUs under the
  1600-line gate (reactor I/O/execute, daemon_config materialize, client transport/error,
  filesystem directory, segment_file IO, runtime_catalog batch/compact, writer_sync,
  read_generation internals/immutable, maintenance evaluate); harden detail-header ODR/includes.
  ADR 0036 remains proposed; `generation_slot_pool` default stays false. WAV-003 revoked
  (audit trail). Documentation paths and `GS-*` implementazione lists updated accordingly.

- Wave 2 (L5 + L1 liaison) hot-path residual: Writer-local vs Reader-hot padding in
  `lane_state.hpp` (`queued_bytes`, telemetry barrier, `LaneMetrics`); skip redundant
  `reader_safe_epoch` acq_rel RMW when unchanged; Writer/caller pause→yield→park wakeup
  ladder; paired volatile GET ≤64 B zero-heap proof (`allocation_fault_tests`); durable_sync
  × token FIFO + put_batch RAW litmus; SPSC hot-producer / slow-consumer fairness. No dual
  TCP ports / io_uring / get-into. ADR 0036 remains proposed; `generation_slot_pool` default
  stays false. Claim ceiling unchanged (architectural prototype). Evidence class `local`.

- Wave 5 (L7): OpenBSD package evidence producer + seal wiring; install-consumer same-SHA ABI
  scaffold; Dependabot SHA discipline in `validate_actions_pins.py`; honest residuals in
  `docs/distribution/wave5-l7-residuals.md` (no fake packages/signatures/tags).

- Wave 4 (L5+L6) daemon/security/operability: handoff exactly-once + concurrent mesh
  tests; TLS `WANT_WRITE` response-flush litmus; abuse principal quotas and security-audit
  counters under concurrency; RST connection-slot release + connection rate-limit window
  reconnect; adversarial soak profile stubs (`hot-key` / `connection-churn` /
  `queue-saturation` / `adversarial-reclaim`) with SIGTERM drain; intentional
  OpenMetrics/Prometheus non-support and Phase 8 remainder unsupported/deferred residuals
  (ADR 0028, restart-only TLS certs). Claim ceiling unchanged (architectural prototype).
  No L1 publish/reclaim ownership in this wave.

- Wave 3 (L4) durability & maintenance: compaction write-amplification / temporary-space budgets
  reject before intent (`GS-PERSIST-AMP-001`); paced pre-intent staging faults and maintenance
  latency/pacing observability (`GS-OPS-DEBT-001`); online compaction and backup
  `storage_exhausted` matrices; fenced backup concurrent with mutation/compaction; platform
  durability evidence path placeholders under `engineering/evidence/platform-durability/`.
  E3/E4 platform rows remain open (rehearsal ≠ certification). Claim ceiling unchanged
  (architectural prototype).

- Wave 0 (L0) assurance hygiene: close stale `GATE-CONCURRENCY-SPEC` residual for ADR 0037
  Phase C (daemon mutation windows + GET barrier already landed; prove paths include
  `mutation_window_tests`). Add living `docs/assurance/evidence-taxonomy.md` (`local` /
  `CI` / `hardware` / `release`; ban promoting macOS unpinned rows to scaling) and
  `docs/assurance/debt-remediation-lanes.md` (L0–L7 / Waves 0–6 map). Claim ceiling
  unchanged (architectural prototype).

- ADR 0037 (accepted): shard execution token + flat combining. Embedded volatile and
  `durable_sync` omit dedicated Writer threads when the async lane is off; callers that
  win `IDLE→EXECUTING` combine already-queued sync work (≤32, no wait-to-fill). Daemon
  keeps per-shard executors; Reactor mutation windows (≤32) raise a GET visibility barrier
  on published `writer_epoch`. Read plane / ACK / fail-closed polarity unchanged. Proofs:
  `shard_combining_executor_tests`, `mutation_window_tests`, paired + durable litmus.

- Structural refactor (behavior-neutral, phases 3–7): extract `fail_closed_state`,
  `mutation_execution` / `mutation_batch`, `publication_coordinator` helpers; nest
  `Lane` by-value aggregates (`lane_state.hpp`) with alignment `static_assert`s;
  centralize Reactor drain decisions in `connection_lifecycle.hpp`
  (`decide_connection_action`, `DecidedOutput`); document internal Writer cause →
  frozen wire mapping in `error-taxonomy-v1.md` §5.1; property tests for
  OVERLOADED ⇔ known-not-committed and sticky decided completion. No wire /
  ACK / fail-closed polarity change. ADR 0036 remains proposed.

- Lab hot-path recon on `94f1307` (`benchmarks/results/local-macos-2026-08-26-head-94f1307/`): PUT end-to-end dominated by Writer ack/handoff (~67%); publication ~14%; `put_batch` ≤32 already amortizes publication (+48% vs single PUT). Rejected wait-based adaptive publication on single-op PUT. Wire `GS_PHASE_GET(index_lookup)` in `PairReadGeneration::get` so phase builds attribute GET lookup cost (~68%).

- Structural refactor (behavior-neutral, phase 0–2): document as-implemented mutation and
  connection lifecycle (`docs/spec/mutation-lifecycle.md`,
  `docs/spec/connection-drain-state-machine.md`); introduce typed `mutation_state`
  (`CommitKnowledge`, `PublicationState`, `MutationLifecycle`, `decide_completion`) with
  transition/characterization unit tests. Sync durable single-op path in
  `ShardPairRuntime::run` mirrors the lifecycle in parallel (bools remain authoritative;
  debug asserts check consistency). Fail-closed durable_sync litmus uses `Site::publish`
  after commit+visibility (success ACK + sticky); before-hook throws stay
  known-not-committed (`DurableSegmentFile::before`). Extract `completion_policy` /
  `mutation_recovery`; sync durable single-op catch applies `plan_sync_durable_exception_*`
  (drain / fail-closed / Status polarity unchanged).
- `snapshot_published_reads` / `capture_published_read`: `ExclusiveIndexQuiesce` before
  Index walk/find on exclusive durable_sync (mutex alone raced mutex-elided mutate
  Index publish → sticky `corrupted_data`). Same protocol as `prepare_get`.
- `flush_dirty_segments` (exclusive durable_sync, no flusher): `ExclusiveIndexQuiesce`
  before dirty sync — mutex+CV alone raced mutex-elided mutate's `cached_file` /
  `mutation_io_active` (lost wakeup / torn handle). Skip waiting on
  `compaction_commit_active` while holding the quiesce gate (self-deadlock).
  Litmus: mid-append PUT + concurrent `flush` → sibling `sequence_conflict`, flush
  completes healthy.
- Paired volatile exclusive Writer: `put`/`erase_locked_published` bump `hot_path_depth`
  and reject while Index quiesce is armed; `Worker::compact` / `Store::verify_index` /
  `get_owned` arm nest-safe quiesce and drain depth before Index touch (mutex alone
  raced unlocked publish → UB / false `corrupted_data`). Litmus: `fault::arm_block(compact)`
  + sibling PUT `sequence_conflict`.
- Mutable Index GET (`prepare_get` / `complete_get` revalidate+TTL reclaim) and
  `snapshot_live_keys`: `ExclusiveIndexQuiesce` (arm `compaction_commit_active` +
  drain `hot_path_depth`) before Index touch on exclusive durable_sync. Mutex-only
  walks raced mutex-elided mutate Index publish → sticky `corrupted_data`. Shared
  helper also used by compaction Phase A and unread TTL probe; gate holders nest so
  overlapping GET / Phase A/C cannot clear `compaction_commit_active` early.
  Litmus: mid-append PUT + concurrent GET → sibling `sequence_conflict`, healthy.
- `capture_published_read`: always take `worker.mutex` (match `snapshot_published_reads`).
  Exclusive durable_sync elision raced compaction Phase A/C Index enumerate/swap and
  could `fail_closed` after a committed PUT. Mutate still elides via `hot_path_depth`.
  Litmus: warm GET under Phase A gate + sibling PUT `sequence_conflict`;
  runtime stays healthy (capture shares the same Worker mutex).
- Unread TTL maintenance probe (exclusive durable_sync): arm
  `compaction_commit_active` and drain `hot_path_depth` before `index.entries()`,
  after releasing the observation catalog lock (no nested shared lock / no catalog
  hold across depth wait). Same Index-ownership hole as compaction Phase A.
  Litmus: mid-append PUT holds depth → sibling PUT `sequence_conflict`, probe
  completes healthy with expired sealed count.
- Compaction Phase A (exclusive durable_sync): arm `compaction_commit_active` and
  drain `hot_path_depth` before `index.entries()`. Mutex-only snapshot raced
  mutex-elided mutates that share only the catalog lock (Index find/prepare/
  publish). Gate clears before Phase B so mutates continue during build.
  Litmus: mid-append PUT holds depth → sibling PUT `sequence_conflict`.
- `rotate_active` post-I/O re-lock: use the mutex-elision predicate
  (`exclusive_writer && flusher_ == nullptr`), not bare `exclusive_writer`.
  Paired durable-group/periodic skipped `worker.mutex` when clearing
  `mutation_io_active` / installing `active_segment`/`cached_file`, racing the
  flusher and sibling mutates. Entry ownership check uses the same predicate.
  Litmus: exclusive_writer + batch, mid-seal stall + same-Worker sibling PUT.
- Compaction Phase C: release Worker/catalog mutexes **before** `hot_path_depth` /
  `mutation_io_active` quiesce waits, then revalidate sequence / pins before
  manifest publish. Holding catalog across the exclusive-Writer depth wait
  deadlocked a mutate that already entered the hot path and needed shared
  catalog after append. Mutation wins via finite `sequence_conflict` + rollback.
  Litmus: exclusive durable_sync, compact intent stall + mid-append PUT.
- Daemon GET under online backup admission fence: refuse as wire `OVERLOADED` (Reactor
  pre-check + `operation_guard_failure` on prepare/complete GET), matching PUT/ERASE —
  not bare `unavailable` → `INTERNAL_ERROR`. Litmus: StallBackup mid-copy concurrent
  Client GET → `resource_exhausted`, key readable after fence lifts.
- Daemon PUT/ERASE under online backup admission fence: reject **before** Writer enqueue as
  wire `OVERLOADED` (mirror maintenance emergency). `try_submit` and exclusive
  `put/erase_durable` also treat an open-lifecycle fence as `resource_exhausted`
  (known not-committed) instead of bare `unavailable` → false `INTERNAL_ERROR` /
  `reconcile_first`. Litmus: StallBackup mid-copy concurrent Client PUT → rejected /
  `resource_exhausted`, not indeterminate.
- `rotate_active` `ExclusiveHotPathPause`: enable only on the mutex-elided exclusive
  Writer path (`exclusive_writer && flusher_ == nullptr`). Paired durable-group /
  periodic keep the Worker mutex and never bump `hot_path_depth` — pausing there
  underflowed depth to `UINT32_MAX` and deadlocked compaction's depth wait against
  rotation's publication wait. Compaction depth wait uses the same predicate.
  Litmus: exclusive_writer + batch flusher, rotation waiting on compaction intent.
- `Store::put_batch` (non-paired / `legacy_mutex`): re-check the maintenance emergency
  gate before each item put (not only at batch entry). Mid-batch / TOCTOU arming rejects
  later siblings as `storage_exhausted` before append — matching single-op `put` and
  `mutate_durable_batch`. Litmus: armed entry reject + `Site::put_batch_gate` mid-batch.
- `classify_ready_loss`: observe paired Writer sticky health (`pair_fail_closed`).
  `Server::ready()` already failed CLOSED when `pair_writers_` was unhealthy, but
  structured `ready.reason` could report `none` — especially on volatile sticky where
  the Store catalog stays `operational`. Litmus: volatile `Site::publish` sticky →
  `ready()==false`, `live()==true`, `classify_ready_loss==pair_fail_closed`.
- `StoreAccess::mutate_durable_batch`: re-check the maintenance emergency gate
  before each sibling mutate (not only at batch entry). Mid-batch / TOCTOU arming
  rejects later siblings as `not_committed` + `storage_exhausted` before append —
  matching single-op StoreAccess paths and `maintenance-controller.md`. Litmus:
  armed entry reject + `Site::durable_batch_gate` mid-batch TOCTOU.
- Polish: Ruby `AsyncClient` classify-before-`reset!` on post-send cancel /
  send-failure (mutate / BACKUP / pipeline), matching Python §6.3. Python
  `AsyncClient.execute_batch` defensive path enriches escaped `GlifiError` with
  `mutation_outcome=rejected` / `bytes_sent=0`.
- `rotate_active`: after seal (or already-sealed / durable create), create and
  publish `not_published` failures preserve `exception_outcome` (`indeterminate`)
  instead of demoting to `not_committed` → wire `OVERLOADED` with a healthy pair.
  Litmus: warm PUT + `segment_full` + `preallocate_segment` reject → `unavailable`,
  pair sticky.
- `rotate_active`: after a committed seal (or durable replacement create), sealed /
  replacement reader-open failures stamp `indeterminate` (via `exception_outcome`)
  and fail-close — not `not_committed` → wire `OVERLOADED`. Litmus:
  warm PUT + `segment_full` + `Site::segment_open` on sealed-reader → `unavailable`,
  pair sticky.
- Official clients (C++, Python sync/async, Go, Ruby sync/async, Perl, Erlang):
  BACKUP post-exchange `validate_response` failure (mismatched `request_id` /
  routing metadata / wrong Worker) enriches `indeterminate` / `reconcile_first`
  with `bytes_sent=frame_len` — same polarity as BACKUP `INTERNAL_ERROR` and
  PUT/ERASE validate-fail. Litmus: FakeServer wrong `request_id` on BACKUP OK.
- `rotate_active`: publication-wait fail-closed and catch-all before a rotation
  write boundary stamp `not_committed` (mirror `mutate()` fail-closed). Catch
  upgrades to `indeterminate` only after a committed seal or indeterminate
  create/publish. Avoids wire `INTERNAL_ERROR` / reconcile when the caller only
  hit `segment_full`/`segment_sealed`. Litmus: `segment_full` + `Site::rotate`
  pre-seal throw → `resource_exhausted`, pair healthy.
- Durable Segment `open` / selected-commit mismatch **before** any Record append
  (mutate path and `rotate_active` open of old active) stamps `not_committed`
  instead of `indeterminate` — matches sealed-reader/replacement open and the
  nearby “not_committed through open” comment. Avoids false sticky fail-close and
  wire `INTERNAL_ERROR` / reconcile for `EMFILE`/`descriptor_exhausted`. Litmus:
  `Site::segment_open` → `descriptor_exhausted`/`resource_exhausted`, pair healthy.
- Python `AsyncClient`: classify post-send cancel (mutate / BACKUP / pipeline)
  **before** `await reset()`, and make `reset()` cancel-safe around
  `wait_closed` — a second `CancelledError` during close must not erase
  indeterminate / `reconcile_first` or fall through to rejected/`bytes_sent=0`
  (§6.3). Litmus: stall-on-PUT cancel with reset raising `CancelledError`.
- Erlang / Perl clients: BACKUP wire `INTERNAL_ERROR` (and Erlang BACKUP
  receive-loss / crash / timeout) stamp `bytes_sent` to the request frame length
  (match C++/Python/Go/Ruby §4 — not `0` / known-unsent). Litmus: FakeServer
  `internal_error_on_backup` asserts `bytes_sent > 0` + `reconcile_first`.
- Official SDKs (Python sync/async, Go, Ruby sync/async, Erlang): post-exchange
  mutate status / empty-OK-value / receive-loss paths stamp `bytes_sent` to the
  request frame length (match C++/Perl §4). Litmus: PUT wire `INTERNAL_ERROR`
  asserts `bytes_sent > 0` + `reconcile_first`.
- `StoreAccess::mutate_durable_batch`: convert pre-mutate `bad_alloc` / unexpected
  throws into per-slot `not_committed` + `resource_exhausted` (pair stays healthy)
  instead of escaping to Writer catch after `durable_mutate_entered=true` (false
  sticky + wire `INTERNAL_ERROR`). Post-entry throws still fill unfinished slots
  indeterminate/`unavailable` and fail-close. Litmus: `Site::durable_batch_pre`.
- Official SDKs (C++, Python sync/async, Go, Ruby sync/async, Perl): `execute_batch`
  group-level pre-admission failure (`ensure_connected` / encode) stamps PUT/ERASE
  slots with `mutation_outcome=rejected` and `bytes_sent=0` (matching Erlang
  `mark_unresolved` / cancel paths). Sibling merge unchanged. Litmus: Worker-1
  rebind fail → slot1 `rejected`.
- Paired sync `put/erase_volatile_published` with `caller_holds_guard` still enforces
  the maintenance emergency gate (`storage_exhausted`). Only the nested
  `OperationGuard` RMW is skipped — closing the mid-batch / TOCTOU hole where
  emergency could arm after `Store::put`/`put_batch` admission but before Writer
  append. Litmus: `caller_holds_guard` under `mutations_rejected`.
- Writer volatile sync/async: known-not-committed rewrite now applies to Store-mutate
  failures that never crossed append (`invalid_reference` rotation →
  `resource_exhausted` / wire `OVERLOADED`). Post-append Index failures stay
  `unavailable` (sticky indeterminate) and are not demoted to OVERLOADED. Litmus:
  `Site::rotate` pre-append + `Site::index_account` post-append.
- Erlang client: `execute_batch` / `execute_worker_pipelines` connect/rebind failure
  stamps only that Worker's planned slots (`failed` / `rejected`, `bytes_sent=0`)
  and still fans out siblings — returns the full slot vector, not a bare `{error}`
  that discarded sibling results. Litmus: Worker-1 `fail_rebind` after conn kill →
  Worker-0 PUT succeeds + Worker-1 rejected.
- Python `AsyncClient.backup` / `_read`: match sync / C++ / Ruby — after any BACKUP
  bytes leave the client (transport/`_SendFailure`, wire `INTERNAL_ERROR`, or
  cancel-after-send), raise enriched `indeterminate` / `reconcile_first` and do not
  blind-retry the same destination. Litmus: FakeServer `drop_on_backup` /
  `internal_error_on_backup` → one attempt + `reconcile_first`.
- Erlang client: I/O-child crash on pending BACKUP enriches `indeterminate` /
  `reconcile_first` like the outer-deadline path (not bare transport /
  `same_request`).
- Perl client: `_mutate` wire `INTERNAL_ERROR` / non-empty OK mutation value, and
  multi-Worker `execute_worker_pipelines` / `execute_batch` fail + status paths,
  enrich `mutation_outcome` / `bytes_sent` like single-Worker `execute_pipeline`
  (and C++). Indeterminate slots no longer advertise bare `new_attempt` /
  `same_request`. Litmus: FakeServer PUT `INTERNAL_ERROR` standalone + two-Worker
  batch → `reconcile_first`.
- Python / Ruby `AsyncClient.execute_batch`: outer cancel after multi-Worker admission
  returns the classified slot vector (keep completed sibling Worker results; classify
  in-flight groups) instead of bare `CancelledError` / `Async::Stop` that discarded
  siblings (§5 / §6.3). Litmus: Worker-0 PUT succeeds, Worker-1 stall-on-PUT, cancel
  the batch task → slot0 succeeded + slot1 indeterminate / `reconcile_first`.
- Writer sync durable single-op fail-closed epilogue no longer overwrites a
  resolved known-not-committed error (`resource_exhausted` → wire `OVERLOADED`)
  to `unavailable` (wire `INTERNAL_ERROR` / reconcile) when the catalog goes
  fail-closed and the drain snapshot fails. Matches catch / durable_group sync
  polarity (keep definitive errors; only fill unresolved / demote unpublished
  success). Litmus: armed `write_some_at` EIO + `Site::drain_snapshot` after
  seed PUT.
- Official SDKs (Python sync/async, Go, Ruby sync/async): `execute_batch` matches
  C++ — a per-Worker group error stamps that Worker's slots `failed` and still
  returns sibling Worker results (non-atomic batch). Previously the first group
  error aborted the whole call and discarded committed sibling slots. Litmus:
  force Worker-1 rebind failure after Worker-0 PUT succeeds.
- Erlang client: outer pipeline / fanout deadline and I/O-child crash return
  per-slot classified vectors (`mark_unresolved` with full planned bytes) —
  mutations `indeterminate` / `reconcile_first` — instead of bare `{error,
  transport}` or all-`failed` + `same_request`. Retains plan metadata on pending
  and fanout children. Litmus: hold-on-PUT pipeline / worker-pipeline timeout.
- Ruby `AsyncClient`: cancel after mutation send classifies indeterminate /
  `reconcile_first` (mutate + pipeline), matching Python §6.3 — not bare
  `Async::Stop`. BACKUP cancel after send likewise. Litmus: stall-on-PUT put /
  pipeline cancel.
- Official SDKs (Python sync/async, Go, Ruby sync/async, Perl, Erlang): pipeline /
  batch per-slot errors now enrich like C++ (`bytes_sent`, `mutation_outcome`,
  `retryability`). An indeterminate pipeline PUT/ERASE no longer leaves a bare
  transport/`INTERNAL_ERROR` with `same_request` / `new_attempt`. Python async
  pipeline cancel after send classifies unresolved slots. Litmus: disconnect /
  INTERNAL_ERROR pipeline slots assert `reconcile_first`.
- Official SDKs (Python sync/async, Go, Ruby sync/async, Perl, Erlang): enrich treats
  `mutation_outcome=indeterminate` as `reconcile_first` even when `bytes_sent` was
  omitted (historically `0`), so a post-send transport failure cannot advertise
  `same_request`. Mutation receive-after-send paths also stamp `bytes_sent` to the
  full frame size. Python AsyncClient: cancel after `write()` / after send classifies
  indeterminate (client-semantics §6.3). Litmus: disconnect-after-PUT + enrich unit
  tests for zero `bytes_sent` + indeterminate.
- Writer sync single durable `catch (...)` never-Store-entered path stamps
  `resource_exhausted` like `bad_alloc` / volatile / async, not `internal_error`.
- Durable Segment `before()` hooks: convert throws to `resource_exhausted` Status
  (known not committed). Runtime mutate no longer stamps `exception_outcome =
  indeterminate` before open/append — a pre-`write_all_at` failure (including a
  throwing `before(write_record)`) stays `not_committed` → Writer/wire `OVERLOADED`,
  not false `INTERNAL_ERROR` / reconcile. Litmus: before-hook throw on first PUT;
  same-key group second item pre-write throw → `resource_exhausted`.
- Writer `catch (...)` never-Store-entered paths (sync volatile chunk / async single)
  stamp `resource_exhausted` like `bad_alloc`, not `internal_error`.
- Writer durable_group: keep the in-flight window through post-`mutate_durable_batch`
  result classification (sync + async two-phase classify-then-stage). A throw after
  mutate returns must stamp Store-entered siblings `unavailable`, not
  `resource_exhausted` / wire `OVERLOADED` while drain still publishes them
  (inverted RAW). `catch (...)` never-entered `before_mutate` is also
  `resource_exhausted`. Litmus: `Site::post_mutate` after a three-key group mutate →
  all `unavailable` + GET hits.
- Writer durable_group catch: never-started later sub-batch placeholders stay
  `resource_exhausted` (known not committed) instead of upgrading to `unavailable`
  once an earlier sub-batch entered mutate. Only the in-flight sub-batch may become
  `unavailable` (write boundary may have been crossed). Sync + async. Litmus: three
  same-key `put_batch` items with throw on second write → first OK, second
  `unavailable`, third `resource_exhausted`.
- Shutdown drain deadline: abandon still-queued (pre-Store) PairWriter mutations as
  `resource_exhausted` **before** hard-closing sockets, so clients observe wire
  `OVERLOADED` (known not committed) instead of bare EOF / indeterminate. Payload
  slot release that would violate FIFO while an earlier Store mutation is in flight
  is deferred until in-order release succeeds; the OVERLOADED flush is not blocked.
  Coalescing waits (`min_records` / burst) break immediately when expire is armed, and
  durable_group later sub-batches re-check expire before Store entry so dequeued-but-
  pre-Store work cannot outlive the deadline. In-flight Store work is still never
  cancelled. Litmus: blocked durable sync + queued sibling; durable_group coalescing
  hold alone → wire `OVERLOADED` + GET miss.
- Official SDKs (Python sync/async, Go, Ruby sync/async, Perl, Erlang): map BACKUP wire
  `INTERNAL_ERROR` to `reconcile_first` / indeterminate (same polarity as C++ after a
  possible committed fenced copy). Avoids advertising `new_attempt` same-destination
  retry that looks like the first backup failed. Litmus: FakeServer INTERNAL_ERROR on
  BACKUP → reconcile_first and a single attempt where counted.
- Wire BACKUP / `Client::backup`: after a successful `backup_to`, report formatting
  failures keep wire `OK` with minimal `status=ok` (never probe `false` →
  `INTERNAL_ERROR`). C++ client maps BACKUP wire `INTERNAL_ERROR` to
  `reconcile_first` / indeterminate (not `new_attempt`). Litmus:
  `Site::backup_report` → OK + restorable destination; client INTERNAL_ERROR mock →
  `reconcile_first` and a single BACKUP.
- Wire BACKUP: preflight OK-report fit against `maximum_output_bytes` / max frame
  before the fenced catalog copy. A post-success report that still will not fit
  returns minimal `status=ok` or `INTERNAL_ERROR` — never `OVERLOADED` (false
  known-not-committed while the destination already holds the backup). Litmus:
  tiny output budget → `OVERLOADED` and destination not created.
- Writer: sync durable_group batch catch no longer upgrades never-Store-entered
  siblings' `resource_exhausted` to `unavailable`. Keep mid-chunk fail-closed /
  rewritten not-committed / sticky errors; only the pre-mutate placeholder may be
  upgraded. Matches volatile sync catch (item prior) and async durable catch.
  Litmus: same-key `put_batch` + `Site::index_account` + `Site::publish` → first OK,
  second stays `resource_exhausted` through catch.
- Writer: sync volatile catch after `store_mutated` no longer upgrades never-Store-
  entered siblings to `unavailable`. Keep mid-chunk `resource_exhausted` (and other
  known-not-committed errors); only unpublished Store-entered nodes stay
  `unavailable`. Fault injection allows concurrent per-site `fail_once` arms.
  Litmus: `Site::mutate` + `Site::publish` on two-key `put_batch` → first OK, second
  remains `resource_exhausted` through catch.
- Writer: sync volatile mid-chunk fail-closed stamps `resource_exhausted` (not
  `unavailable`) for siblings that never entered Store after another lane sticky-
  closes; sync incremental-merge backpressure matches async (`resource_exhausted`).
  Avoids wire `INTERNAL_ERROR` / client `indeterminate` for known-not-committed
  batch siblings. Litmus: `Site::mutate` after first same-shard put_batch item →
  first OK + GET hit, second `resource_exhausted` + GET miss.
- READY observes online backup admission fence: `Store::admissions_open` /
  `Server::admissions_open` reflect `close_admission`; `ready()` and
  `classify_ready_loss` (`admission_fenced`) fail while BACKUP holds the fence,
  matching wire-protocol "Store admission open". HEALTH/live unchanged.
  Litmus: stall `backup_to` mid-segment-copy → `ready()==false`, then OK after resume.
- TLS post-accept drain: after adopting a TLS connection, call `read_ready` once
  so application records already in OpenSSL after `SSL_accept` (TLS 1.3
  coalescing) are processed without waiting for a fresh edge-triggered readable
  event. Connection-local drain failures close the peer only (accept loop stays
  up). `TlsSession::pending()` wraps `SSL_pending`. Litmus: `SSL_connect` then
  immediate INIT → OK without a second client write.
- TLS `SSL_write` `WANT_READ`: distinguish `want_read` / `want_write` on the wire
  I/O result, keep read interest even after half-close, opportunistically
  `SSL_read` then retry write once (edge-triggered pollers do not re-fire).
  `read_ready` retries pending output on TLS block. Avoids stranded committed
  ACKs that looked like client timeouts / indeterminate. Litmus:
  `Site::tls_write_want_read` → PUT OK still delivered.
- Shutdown BIND handoff drain: `ConnectionHandoffMesh::stop_accepting` on
  `request_stop` (before executors observe stop), pending cells keep
  `idle_for_shutdown` false, and force-close / post-drain
  `reject_pending_handoffs` surfaces wire `OVERLOADED` instead of destroying
  buffered BIND OK with bare EOF. Litmus: pending cross-Worker BIND +
  `close_all_connections` → `OVERLOADED`; mesh unit coverage for stop/pending.
- Writer: broaden known-not-committed wire rewrite — any Reactor `INTERNAL_ERROR`-bucket
  code (`corrupted_data`, `internal_error`, `invalid_record`, …) becomes
  `resource_exhausted` → wire `OVERLOADED` / client `rejected`. Codes already mapped
  to rejected polarity are unchanged. Litmus: inject `write_record` `corrupted_data`
  and `internal_error` → `OVERLOADED`, GET miss.
- Writer: already-queued mutations rejected before Store entry after sticky fail-closed
  stamp `resource_exhausted` (not `unavailable`). Catalog `not_committed`+`unavailable`
  is rewritten the same way as known-not-committed `io_error`. Avoids wire
  `INTERNAL_ERROR` / client `indeterminate` for siblings that never linearized.
  Taxonomy §5 updated. Litmus: async same-key sibling completion is
  `resource_exhausted`; wire two-connection same-key + `Site::capture` → first OK,
  second `OVERLOADED`.
- Writer: rewrite known-not-committed durable `ErrorCode::io_error` to
  `resource_exhausted` before completion (sync/async single and batch). Pre-write
  append rejects used to keep `io_error` → Reactor `INTERNAL_ERROR` → client
  `indeterminate` / `reconcile_first` despite authoritative not-committed. Same
  polarity as `segment_full` → wire `OVERLOADED`. Taxonomy §5 note updated.
  Litmus: inject `write_record` `io_error` → wire `OVERLOADED`, GET miss.
- Reactor BIND handoff: honor `poller_.remove` failures (retry `EINTR` in epoll/kqueue
  backends). Do not move a socket that is still registered on the source Reactor —
  dual registration spun the source executor on stale tokens. On remove failure,
  replace buffered BIND OK with wire `OVERLOADED`, drain, and close locally.
  Litmus: `Site::poller_remove` on cross-Worker BIND → `OVERLOADED`, target has
  zero connections, peer closes.
- `Store::backup_to`: after acquiring `compaction_mutex`, re-close admissions and
  drain active operations before the catalog copy. A concurrent backup that waited
  on the mutex used to copy after the first backup's `resume_admission_if_open`,
  violating the fenced-snapshot guarantee while still returning OK. Litmus: stall
  first backup mid-segment-copy, overlap a second backup, assert PUT is refused
  during the second backup's manifest copy.
- Reactor wire `BACKUP`: require successful `INIT` + `BIND_WORKER` before running the
  fenced backup path (`NOT_BOUND` otherwise). Unbound BACKUP was not a documented
  pre-handshake exemption and could stall a Reactor when authz was off. Spec
  `wire-protocol-v2` Bound-state list updated. Litmus: raw BACKUP before INIT →
  `NOT_BOUND` and no destination created; INIT+BIND+BACKUP still OK.
- Official SDKs (Python sync/async, Go, Ruby sync/async, Perl, Erlang): do not
  blind-retry wire `BACKUP` after any request bytes were sent — same polarity as
  `Client::backup`. Lost OK then retry falsely failed with "destination not empty".
  After send, return indeterminate / `reconcile_first`. Python litmus: drop-on-backup
  FakeServer asserts one BACKUP frame.
- `Client::backup`: do not blind-retry the generic `read()` loop after any request
  bytes were sent. Online BACKUP is not idempotent to the same destination (requires
  pristine dir); a lost OK then retry used to surface "destination not empty" as a
  false failure. After send, return indeterminate / `reconcile_first` like in-flight
  mutations. Litmus: mock accepts one BACKUP and drops the response — only one BACKUP
  frame, error carries `mutation_outcome=indeterminate`.
- Reactor `run_once` poller flags: handle `hangup` before raw `error`, and only
  hard-close on `error` when `SO_ERROR` is sticky/non-zero (best-effort drain first).
  Co-reported `EPOLLERR|EPOLLRDHUP` / kqueue `EV_EOF` used to error-close after a
  partial writable flush and discard remaining decided bytes. Litmus: large PING
  with small client `SO_RCVBUF` + `SHUT_WR` still delivers the full PING OK.
- Reactor INIT: catch identity-value `std::bad_alloc` (`Site::init_identity`), leave
  `initialized` false, and queue wire `OVERLOADED` instead of escaping to executor
  fail-stop. No store work is committed on INIT. Litmus: INIT fault → `OVERLOADED`,
  server stays live; a second connection completes INIT+BIND.
- Reactor `read_ready` input append: catch `std::bad_alloc` (`Site::input_buffer`) and
  input high-watermark overflow; isolate to the connection. If decided output is pending,
  `close_after_flush` + drain via `write_ready` instead of returning Status that makes
  `run_once` hard-close and discard the ACK. Litmus: large PING (small client `SO_RCVBUF`)
  + follow-up with input-buffer fault still delivers PING OK; server stays live.
- Reactor `queue_response` / scatter `queue_owned_response`: translate `std::bad_alloc`
  (and `Site::response_queue` litmus) to `resource_exhausted` instead of escaping to
  executor fail-stop. Mutation completions keep close-without-ACK polarity — no invented
  `OVERLOADED` after a possibly committed write. Litmus: durable PUT + ACK-buffer fault →
  empty wire / peer close, server stays live+healthy+ready, reconnect GET sees the value.
- Pre-Store Writer lane expiry / merge-retire pressure / force-expire stamps
  `resource_exhausted` (wire `OVERLOADED`, known not committed) instead of
  `unavailable` (`INTERNAL_ERROR` / reconcile). Sticky/post-commit paths keep
  `unavailable`. Existing deadline litmus expects `overloaded`.
- `Server::live()` / HEALTH: process started and no executor `failed_` only —
  pair sticky fail-closed no longer fails liveness or auto-exits the daemon loop.
  `ready()` still requires `pair_writers_->healthy()`. `DurableRuntimeCatalog::close`
  on already-fail-closed durable_sync (no flusher) returns success so `join` is
  clean. Litmus: sticky → live+HEALTH OK, READY fail, join OK.
- BIND OK / mutation·cold-read completions: flush (`write_ready`) before
  `process_frames` so a pipelined decode failure cannot discard a decided response
  as silent EOF. Litmus: BIND + trailing bad-version frame still yields BIND OK.
- Half-close / completion drain: `write_ready` processes residual input before
  `peer_read_closed` teardown; EOF/hangup only close when input+output are idle.
  Mutation/cold-read completions and same-executor BIND no longer call
  `process_frames` while decided bytes remain after `EAGAIN` (decode error used to
  close and discard the ACK). Litmus: pipelined PUT+PUT then `SHUT_WR` ACKs both
  and values remain visible.
- `read_ready`: same decided-response drain as `write_ready` when `process_frames`
  fails after queuing abuse `OVERLOADED` / authz deny — trailing decode used to
  close as silent EOF and hide the rate-limit signal. Litmus: budget-exhausted
  PING + bad-version frame still yields `OVERLOADED`.
- Partial-frame `--request-timeout-ms`: when decided output remains (EAGAIN), set
  `close_after_flush` and drain before teardown — idle timeout already required
  `!has_pending_output`, but partial timeout did not and could discard a prior ACK.
  In-flight timeout best-effort flushes prior decided bytes once then hard-closes.
  Litmus: large PING + small client `SO_RCVBUF` + trailing partial frame still
  delivers the full PING response.
- Reactor `response_status`: `segment_full` / `segment_sealed` / `arithmetic_overflow`
  map to wire `OVERLOADED` (known not committed), not `INTERNAL_ERROR` (reconcile).
  Same capacity posture as `storage_exhausted`. Litmus: unit map + durable PUT with
  injected `write_record` → `segment_full` yields wire `OVERLOADED`.
- Shutdown-deadline `close_all_connections`: best-effort `write_ready` before hard
  close so a decided ACK the kernel can accept is not discarded when drain times out
  (join still reports drain failure).
- BIND orphan-handoff best-effort OVERLOADED `send` uses `reactor_detail::send_flags()`
  (`MSG_NOSIGNAL` on Linux), matching the hot write path — a peer-reset during reject
  must not raise SIGPIPE.
- Catalog flush-after-abandon gates use sticky `healthy_` (not `healthy()`):
  `close()` sets `closed_` before the final flush, and `healthy()` is false while
  closed — the old gate skipped deferred `sync_record` and returned success,
  losing sticky final-flush failures. Litmus: deferred put + injected
  `sync_record` failure on `close()` → `io_error`, sticky on repeat close.
- Sync durable batch catch-path drain: remove presence-based
  `ack_attempted_after_visibility` (put-hit on a pre-existing key falsely
  success-ACK'd unapplied puts). Only sticky Index-committed items upgrade after
  drain. Litmus: `put_batch` overwrite of an existing key that throws mid-mutate
  keeps error ACK while GET still sees the old value.
- `close_after_flush` (BIND handoff OVERLOADED drain): refuse new frames / clear
  input so a pipelined BIND cannot append OK after OVERLOADED.
- Reactor maps Store `ErrorCode::unavailable` to wire `INTERNAL_ERROR` (not
  `OVERLOADED`). Sticky/fail-closed and post-commit Writer paths may already have
  linearized; `OVERLOADED` would falsely tell clients the mutation is known-not-
  committed. Admission/capacity still use direct `OVERLOADED` /
  `resource_exhausted`/`storage_exhausted`. Litmus: `response_status` unit map +
  durable `Site::publish` sticky PUT → `INTERNAL_ERROR` on the wire.
- TCP/Unix `accept` and post-accept `configure_*` failures no longer fail the
  Reactor executor (same isolation posture as TLS handshake reject); retry on the
  next listen readiness.
- Reactor `BIND_WORKER` handoff: on a full per-Worker MPSC queue, restore the
  connection on the source Reactor (do not free the slot / bump generation before
  enqueue), replace the buffered OK with wire `OVERLOADED`, best-effort flush, then
  close — no silent success ACK destroyed with the socket. Litmus: prefill Worker 1
  handoff ring (capacity-1 → ring of 2), pump only Reactor 0, cross-Reactor BIND
  observes `overloaded` then EOF. Destination connection-table exhaustion and
  adopt `SO_SNDBUF`/poller failures likewise best-effort `OVERLOADED` then close
  (accept-flood stays silent drop); queue-full path uses `close_after_flush` so
  pending OVERLOADED is not cleared on would-block. Litmus: Worker 1 max-connections=1
  filled, BIND from Reactor 0 observes `overloaded` then EOF.
- ADR 0036 (proposed): generation slot-pool publish/reclaim design bar and V1–V14
  verification matrix before any production protocol swap. Default paired path
  unchanged (`shared_ptr` + ReadLease). Prototype gates V1/V2/V3/V6/V7/V9/V13
  evidenced. Production-baseline: V3 overwrite-storm (TSan), V4 durable refresh/cold
  pin, V6 sticky fail-closed (sync `healthy_` reject + couple when durable marks
  itself unhealthy), V8/V10, V14 full sync crash matrix (91 checkpoints). See
  `docs/adr/0036-generation-slot-pool-publish.md` and
  `benchmarks/results/local-macos-2026-08-02-adr0036-*/`.
- Paired Writer: sticky fail-closed on exceptions after durable mutate has begun
  (and on durable committed-with-error / indeterminate without staged publication),
  aligning async batch/single-op paths with the sync `publication_required` rule. Sync
  `mutate`/`mutate_batch` reject when `!healthy_`; Writer couples pair `healthy_`
  when the durable catalog becomes unhealthy without a thrown exception. Volatile
  sync multi-chunk drains (`put_batch` >32) and async durable coalesced sub-batches
  stop mutating after fail-closed instead of continuing later chunks/items. After a
  later-item publication failure, async and sync durable batches still drain-snapshot
  publish committed siblings (`allow_fail_closed` when durable already self-closed)
  or publish already-staged volatile siblings before sticky close — earlier commits
  are not left unpublished with an aborted ACK. Sync `durable_sync` / single-op
  Writer path drain-snapshots before sticky close on capture/publish failure
  (and batch catch drains when `durable_mutate_entered`), matching async; ACK is
  success when the drain published a clean commit. Sync single-op also
  ACK-after-visibility for `committed+error` / indeterminate after drain (put hit /
  erase miss), marks generation published before reclaim, and preserves success ACK
  across Writer catch once authority is published. Sync durable batches and async
  durable (batch + single-op) likewise upgrade sticky Index-visible `committed+error`
  completions after a successful drain-snapshot (only tracked committed sticky items —
  not incidental same-key put-hit).   Sync durable batch items start as not-processed
  (not default success); catch-path drain promotes only sticky Index-committed items
  (never presence-upgrade of unprocessed attempted puts — pre-existing keys would
  false-ACK). Volatile sync chunks mark generation published
  before reclaim and preserve success ACK across catch (`Site::publish` litmus),
  matching durable. Async Writer likewise ACK-after-publish for staged indices
  (volatile sticky sibling publish and happy-path), skips error-ACK in catch when
  already staged, and preserves success completions across post-publish catch
  (`Site::publish` async volatile/durable put+erase litmus). Sync durable single-op
  catch keeps any already-resolved ACK polarity (including visibility-failed errors)
  instead of promoting them to success when a generation was published. Catalog Index
  publish advances
  `durable_through` before secondary accounting/hot so flush/finalize keeps success
  for already-indexed sequences (`Site::index_account` litmus, including async
  durable_group put/erase); flush/wait failures after Index authority return
  `committed` rather than indeterminate. Index/hot publication in
  `flush_worker_batch` and non-strict mutate accounting catch allocation throws
  into fail-closed `publication_failed` / `committed+error` (no escape with
  partial Index + uncleared pending). `writer_durable_through` takes the Worker
  mutex so batch finalize cannot under-read Index coverage. Async durable
  upgrades clean-commit completions to success after a successful drain-snapshot (no
  visible key + error ACK), and drain-snapshots before sticky close when happy-path
  `publish_incremental` fails. After sticky close, immutable
  published-generation GETs (`prepare_published_get` / borrowed complete) remain
  servable so success-ACK'd siblings keep RAW via `Store::get`; mutable Index GET
  and mutations stay rejected. Unflushed writer-batch siblings
  abandoned when a later item clears `pending_group` are rewritten to indeterminate
  (gate on `durable_through` for both commit fail and unhealthy no-op — already-flushed
  siblings keep success ACK + drain-snapshot; orphaned Segment pending still fails
  commit). Async durable catch preserves staged sibling success completions. Indeterminate
  emitters that previously left durable healthy (deferred TTL drain on mutate, rotate
  preflight corruption) now abandon/fail-close; `mutate_durable_batch` also stops
  later siblings on indeterminate/committed-with-error. Refresh/merge skip when pair
  `!healthy_`. Sync volatile chunks also stop mid-chunk when another lane sticky-fails.
  Pre-I/O `!healthy` returns `not_committed`. Prevents unpublished committed durable
  state, inverted RAW, and post-fail-closed mutation.
- Record encode: `encode_record(out, input, encoded_size)` avoids a second
  `encoded_record_size` pass on Segment append and durable encode-scratch paths.
  Bytes unchanged; release validates input + extent; debug asserts size match.
  Lab attribution (`GLIFISTORE_HOT_PATH_PHASES`): `encode_copy` ~66 ns,
  `index_publish` ~158 ns inside Writer apply; `ack` still ~64% of PUT section.
  Evidence: `benchmarks/results/local-macos-2026-08-02-encode-size/`.
- Embed `DeltaState` in the `make_shared` generation allocation (one heap block
  per publish for shell + delta). Wire Writer `worker_apply` / `publish` phase
  scopes on sync volatile PUT. Lab `store_put` ~370–378 k vs prior plain ~344 k
  on Apple M4; affine within prior interleaved noise. Phase dump: apply ~381 ns,
  publish ~640 ns, ack wait ~3 µs (scheduling residual). Rejected Writer
  post-sync spin, sync-before-merge reorder, and conditional `writer_waiting`
  notify (A/B noise / no reliable win; see
  `benchmarks/results/local-macos-2026-08-02-writer-waiting/`). Evidence:
  `benchmarks/results/local-macos-2026-08-02-embed-delta/` and
  `.../local-macos-2026-08-02-sync-first/`.
- DeltaArena segment pins: store `SegmentPtr` by value in a `deque` (stable
  addresses for `DeltaRecord::pin`) instead of `unique_ptr<SegmentPtr>` per new
  pin — one fewer heap allocation on cold pin insert; steady-state same-segment
  hits unchanged.
- Co-allocate `PairReadGeneration` object + `shared_ptr` control block via
  `make_shared` / private helper (no freelist, no custom deleter).
  Interleaved affine PUT A/B neutral; mild 1t PUT gain. Lab:
  `benchmarks/results/local-macos-2026-08-02-make-shared-gen/`.
- ADR 0035 (rejected): TLS `PairReadGeneration` shell freelist under the existing
  publish protocol. Same-machine A/B on Apple M4 showed mild 1t PUT gain but
  **−16% worker-affine PUT 2t**; not shipped. Evidence under
  `benchmarks/results/local-macos-2026-08-02-gen-shell(-ab-baseline)/`.
- Add `Store::put_batch` with paired Writer sync coalesce (≤32 mutations per
  publish, stack chunking, no early ACK). Same-shard batches share one epoch;
  cross-shard groups run independently. Lab `store_put_batch` ~527 k ops/s vs
  single `store_put` ~372 k on Apple M4 (`macos-release`). FIFO within a batch
  is tested. Single-op PUT remains publish-bound.
- Paired sync Writer: skip nested `OperationGuard` on `put_volatile_published` when
  the embedded caller already holds admission (`PublishedAdmission::caller_holds_guard`).
  The maintenance emergency gate remains enforced on that path (not skipped). Async
  path unchanged. No early ACK; lab `store_put` remains publish-bound (~400 k ops/s
  on Apple M4). Rejected for now: Delta COW freelist (measured regressions).
- Hot-path performance program (lab, macOS Apple Silicon): disableable phase
  attribution (`GLIFISTORE_HOT_PATH_PHASES`), GET path consolidation + ReadLease
  without Writer wake + 64 B `OwnedValue` SSO, bounded adaptive spin / proportional
  reclaim on paired sync PUT, TCP phase scopes. Lab results under
  `benchmarks/results/local-macos-2026-08-01-perf/` and
  `docs/architecture/hot-path-performance-2026-08-01.md`. get_copy ~3.5 M ops/s
  (286 ns median); TCP w4 p128 no longer regresses vs p32 in this run. PUT still
  Writer-ack bound (~370 k ops/s). Prototype claim ceiling unchanged.
- Record full local macOS release benchmark suite at
  `benchmarks/results/local-macos-2026-08-01/` (`629bc68`, Apple M4, `macos-release`,
  CI-equivalent matrix). Absolute numbers are prototype/lab evidence, not production
  capacity claims.
- Add `.gitleaks.toml` allowlist for public SipHash/SHA-256 IV false positives and
  reword durable-runtime-catalog phrasing so post-squash whole-tree secret scans
  stay green.
- CI green: crash harness self-SIGKILL at FS boundary (closes Linux flusher/reactor
  race after checkpoint marker), IPO on LTO test/tool binaries, Ruby TLS pin
  fallback, atomic linearizability `next_id_`, paired sync Writer pre-merge under
  delta pressure, durable-group smoke PASS regex 1–4, single-worker parallel bench
  ops 50k, Perl `tar|head` SIGPIPE fix, Erlang OTP25 hex + `cacertfile` TLS + safe
  inet reason formatting, OpenBSD Go `-buildvcs=false`, interop TLS uses CA+leaf
  (OTP-strict), hosted benchmark report keeps Δ but drops hard fail gate, prototype
  Reactor GET telemetry assert softened for ARM flake.
- Ship portable mdoc(7) manual pages for every Runtime install binary
  (`glifistored.8`, maintenance tools in section 1, overview `glifistore.7`), installed via
  `GNUInstallDirs` `MANDIR` for Linux/macOS/FreeBSD/OpenBSD; `scripts/validate-manpages.sh` + CI
  job; optional `GLIFISTORE_COMPRESS_MANPAGES`.
- CI green fixes: lychee 0.24 timeout/exclude_path, actionlint labels, CodeQL Go manual
  build, OpenBSD SIGPIPE + daemon-CLI ctest scope + skip three qemu-flaky reactor tests,
  FreeBSD exclude crash/fault labels, durable-group smoke threads 32→4, BlockingFileSync
  waits 15s, paired Writer notify-after-snapshot (ASan stack-use-after-return), skip
  allocation-fault suite under TSan, clang-format 21.1.8 sweep.
- Copyright / license compliance hardening: exact SDK `LICENSE` sync, per-SDK `NOTICE`,
  `LICENSES/BSD-3-Clause.txt` + `reuse lint` in `ci-license-check`, TLS redistributor notes
  (`docs/legal/tls-redistribution.md`), SipHash paper attribution across SDKs/ADRs, packaging
  asserts for LICENSE/NOTICE in Python/Ruby/Perl/Go/Erlang artifacts, and expanded
  `THIRD_PARTY_NOTICES.md` (vmactions, Syft).
- Copyright / license compliance: expanded `NOTICE`, added `THIRD_PARTY_NOTICES.md` and
  `docs/legal/licensing.md`, `REUSE.toml` path annotations, CMake install of third-party notices,
  synced `sdk/erlang/LICENSE`, and hardened `scripts/ci-license-check.sh` (notice presence, SDK
  LICENSE sync, Go/Python scans).
- Close HAZ-026 reclaim-starvation residual with adversarial proofs: MaintenanceController unit
  coverage that a `reclaim_threshold` skip advances to a reclaimable peer Worker, plus durable
  Store integration that a live-only Worker cannot pin the observe cursor and starve overwrite
  debt on a peer (`maintenance_controller_tests`, `compaction_builder_tests`). Residual:
  multi-hour adversarial fairness soak / group-commit per-Worker starvation telemetry remain open.
  No `e3_certified=yes`.
- GitHub CI surface expansion (prototype claim ceiling unchanged): OpenSSF Scorecard,
  dependency-review, actionlint + pin validator, CodeQL Python/Go/Actions, Linux ARM64 +
  `unix-release` / `unix-release-lto` jobs, checksum-pinned Syft + C++ install-prefix SBOM,
  GitHub Release attach on tags, docs link check, diagnostic coverage, SDK license hygiene,
  Dependabot pip/gomod, and branch-protection settings checklist
  (`docs/assurance/github-branch-protection.md`). Hosted Actions billing remains an honest residual.
- Paired adoption naming: prefer shard-pair / Reader / Writer language in operator telemetry docs
  (`observability` lane/batch surfaces, durable-tcp hot-cache wording) while keeping Manifest/wire
  `worker_count` and CLI `--workers` as 0.1.x aliases.
- Paired adoption P1 residual honesty: optional Linux I/O backend (`io_uring` / batched
  completion) **deferred** for 0.1.0 without a measured queue/syscall win that preserves write
  ordering; refresh Delta COW / get-into reject / Linux harness residual status in
  `paired-shards-plan`, lab prototype “Prossimo gate”, and durable cold-read follow-ups. No
  fabricated `glifistore-linux-perf` evidence; no `e3_certified=yes`.
- Paired adoption Fase 0 honesty: dual-path inventory on the production roadmap
  (`docs/v1-production-roadmap.md` — public owning `Store::get` vs daemon `ReadGeneration`,
  `pair_writer_stats`, `--shard-pairs` / `--workers` alias, lab-only `experimental/`). Complements
  the earlier CHANGELOG inventory under the sole-runtime docs cutover.
- Daemon request/idle timeout vs client contract: prove `--request-timeout-ms` closes
  stalled partial frames and in-flight cold-read waits (`abuse_request_timeout_closed`,
  cancel-on-close) without cancelling admitted durable Store mutations
  (`server_reactor_security_tests` / `server_reactor_durable_tests`); document in
  client-semantics §6.2 and durable-tcp-daemon. Closes GATE-CONCURRENCY-SPEC /
  `GS-PROTO-WIRE-001` residual on daemon cancellation/deadline beyond client contract.
  TLC/checker history residuals unchanged; not N−1 fixtures; not E3.
- HAZ-021: real `glifistored` exec mid-BACKUP kill matrix via env-gated crash hooks
  (`GLIFISTORE_CRASH_TEST` / `GLIFISTORE_CRASH_KILL_AT` / `GLIFISTORE_CRASH_CHECKPOINT_DIR`)
  and `glifistore_crash_backup_daemon` (`copy_backup_segment` / `copy_backup_manifest` /
  `sync_backup_destination`). Incomplete dest fails verify; source reopens healthy. Lab-only;
  production leaves hooks unset. Still not zero-fence (ADR 0034); durable remains sync write-through.
- Runtime SDK online `BACKUP` interop smoke (`scripts/test-sdk-backup-interop.sh`): durable
  `glifistored` + typed `backup()` for Python/Go/Perl/Ruby/Erlang; wired into CI `sdk-clients`
  (`BACKUP_INTEROP_REQUIRE_ALL=1`). Closes the symbol-only residual from
  `assert-sdk-backup-helpers.sh` (still fenced, not zero-fence).
- C++ `Error` carries portable `mutation_outcome` (`rejected` / `indeterminate`) on failed
  TCP-client mutations and pipeline mutation positions; `portable_mutation_outcome(wire_status)`
  matches the taxonomy fixture. Closes the last `GS-PROTO-ERROR-001` residual on C++ Error.
- HAZ-022: synthetic incompatible restore/open matrices — checksum-valid future Manifest
  format and future pinned Record version refuse verify/restore/`Store::open`; truncated
  Manifest and Worker-count mismatch after restore also fail closed
  (`tests/unit/store_backup_tests.cpp`). `STORE-FUTURE-REQUIRED` evidence updated; tagged
  N−1 permanent fixture drops remain a release residual.
- `GS-PROTO-ERROR-001`: unknown wire status maps uniformly to category `protocol` (preserve numeric
  `wire_status`; mutation `indeterminate`) across C++ and official SDKs; fixture case
  `unknown_wire_status` (99). Codecs still reject unknown statuses after buffering the frame.
- Offline migrate resume hardening (ADR 0024 / `GS-OPS-MIGRATE-001`): real mid-copy interrupt via
  destination `FilesystemHooks` then resume; fail-closed corrupt/mismatched/orphan checkpoints;
  N↔N-1 `STORE-WORKER-RESHARD` evidence points at unit tests.
- HAZ-021: wire/reactor BACKUP process-kill via in-process Server + `Client::backup`
  (`glifistore_crash_backup_wire`). Incomplete dest fails verify; source reopen + wire GET after
  Server restart.
- HAZ-021: Store process-kill mid-backup via `FilesystemHooks`
  (`copy_backup_segment` / `copy_backup_manifest` / `sync_backup_destination`) and
  `glifistore_crash_backup`. Incomplete dest fails verify; source reopens healthy.
- Surface `source_crc_scanned` / `destination_crc_scanned` on wire `BACKUP` ASCII and
  `glifistore_backup_store` text/JSON reports. Clarify ops docs: online fenced backup is supported;
  zero-fence hot backup is not.
- HAZ-021: incomplete backup destinations fail verify/restore; failed online backup leaves the live
  Store usable (`tests/unit/store_backup_tests.cpp`).
- Online fenced backup: keep only **structural** source verify under the admission fence; run
  committed CRC scan on the destination after admissions resume (promotion gate). Offline backup
  still CRC-scans the source once before copy. Report `source_crc_scanned` /
  `destination_crc_scanned`.
- Parallelize catalog Segment file copy during offline/online backup (bounded workers, Manifest
  last). Report `segment_copy_workers`. Completes ADR 0034 fenced-path incremental (shorter fence +
  copy parallelism); still not zero-fence.
- Shorten online `Store::backup_to` admission fence: resume writers after catalog copy; run
  destination verify outside the fence. Report `admission_fence_ns` / `catalog_copy_ns` /
  `destination_verify_ns` on backup reports and wire `BACKUP` ASCII output (still not zero-fence;
  ADR 0034).
- ADR [0034](docs/adr/0034-zero-fence-hot-backup-deferred.md): freeze design requirements for
  zero-fence hot backup; 0.1.x keeps offline + online fenced paths only.
- Docs P1: [operations handbook](docs/operations/README.md) and
  [ADR 0033](docs/adr/0033-online-rebalance-deferred.md) (online rebalance design frozen,
  implementation deferred past 0.1.x).
- Docs P1: normative [backup-restore-v1](docs/spec/backup-restore-v1.md) and
  [tcp-client-conformance-v1](docs/spec/tcp-client-conformance-v1.md).
- Docs P1: [observability reference](docs/operations/observability.md) for HEALTH/READY/STATS
  needles, JSON lifecycle logs, and `--dump-config` (no metrics exporter claimed).
- Docs P1: operator [compatibility-and-migration](docs/operations/compatibility-and-migration.md)
  manual and [release-checklist](docs/assurance/release-checklist.md); WAV-001 size waiver revoked
  after production and test suite splits under the structure line budget.
- Close WAV-001: split remaining oversized test suites
  (`maintenance_controller_*`, `server_reactor_*`, `persistence_recovery_*`) under the
  structure line budget; revoke the size waiver.
- Reduce WAV-001 surface: split `src/persistence/runtime_catalog.cpp` into
  `runtime_catalog_detail.hpp`/`.cpp`, `runtime_catalog_ops.cpp`, and
  `runtime_catalog_maintenance.cpp` (each under the structure line budget).
- Reduce WAV-001 surface: split `src/server/reactor.cpp` into `reactor_detail.hpp` +
  `reactor_dispatch.cpp` (I/O/lifecycle vs frame/dispatch path).
- Reduce WAV-001 surface: split `src/store/store.cpp` into `store_impl.hpp`,
  `store_access.cpp`, and `prepared_cold_read.cpp` (each under the structure line budget).
- Assurance Phase E: performance/soak/overload budgets
  (`engineering/performance/budgets.yaml`, `validate_perf_budgets.py`) linked to
  `GATE-PERFORMANCE` / `GATE-SOAK` / `GATE-OPS-RUNBOOKS` (`GS-PERF-BUDGET-001`,
  `GS-OPS-SOAK-001`). Absolute hardware thresholds remain `specified_waiting_for_runner`.
  Final honest summary: `docs/assurance/final-engineering-report.md` (claim ceiling stays
  architectural prototype).
- Assurance Phase D: N↔N-1 compatibility matrix (`engineering/compatibility/n-n1-matrix.yaml`),
  SHA-pinned GitHub Actions (`validate_actions_pins.py`), release claim schema/packaging
  (`engineering/claims/`, `scripts/package-release-claim.sh`), and requirements
  `GS-COMPAT-NN1-001` / `GS-SUPPLY-ACTIONS-001`. Residual: permanent tagged N−1 fixture drops.
- Assurance Phase C: split root `CMakeLists.txt` via `add_subdirectory` for
  `src`/`tools`/`tests`/`benchmarks`/`fuzz` (installed `GlifiStore::*` aliases unchanged);
  add `engineering/build/dependency-matrix.yaml`, structure debt thresholds, waivers (`WAV-001`),
  and CI validators `validate_cmake_deps.py` / `validate_structure_debt.py`.
- Paired embedded Store gate snapshot (ADR 0032 T5): macOS-release Zipf durable parallel GET
  p50/p99 recorded in `docs/benchmarks/paired-embedded-store-gates-2026-08-01.md`; full ctest
  37/37 green. Fix `ShardPairRuntime::Lane` member init order warning.
- Paired exclusive Writer mutex-elision (ADR 0032 T2): durable `mutate` / `capture_published_read`
  skip the Worker mutex when `exclusive_writer` and no background flusher (`durable_sync`);
  compaction waits on `hot_path_depth`. Volatile paired Writers keep generation-only publication
  without `mutex_` on the hot path (debug assert). Compaction/verify/backup/catalog-refresh
  snapshots retain locks. Catalog shared lock on mutate/capture remains (pin lookup).
  Crash recovery harnesses open with deprecated `legacy_mutex` so TSan crash matrices are not
  dominated by paired Writer startup. `legacy_mutex` ctest path documented in
  `docs/development/test-strategy.md`.
- ADR 0032: paired Reader/Writer concurrency is the product default for embedded `Store::open` as
  well as `glifistored` (amends ADR 0031/0005/0009 concurrency notes). Persistence v1 and wire v2
  unchanged; public owning `Store::get` unchanged. Docs aligned
  (`concurrency-memory-model`, `worker-model`, `public-api-contract`, glossary). Deprecated
  `legacy_mutex` escape hatch documented for 0.1.x removal in 0.2; mixing legacy mutators with a
  paired Writer on one Store is refused / UB.
- Embed `ShardPairRuntime` in `glifistore_core`: `Store::open` defaults to paired (Writer thread +
  published `ReadGeneration` per shard). Public `get` adopts the generation (durable cold reads
  complete synchronously); `put`/`erase` hand off to the Writer. Durable hot-cache admission is
  disabled in paired mode (generation-only). `glifistored` opens the same paired Store and uses a
  thin `PairWriterPool` adapter (no second publication spine). `src/experimental/paired_*` remains
  lab-only.
- Document paired Reader–Writer as the sole `glifistored` 0.1.0 runtime (ADR 0031); the volatile
  engine under `src/experimental/` remains lab-only and is not a second selectable daemon. Inventory:
  public `Store::get` keeps owning pins; daemon GET borrows a Reader-local `ReadGeneration`;
  `Server::pair_writer_stats()` is the paired mutation-lane surface while Manifest/wire
  `worker_count` and CLI `--workers` stay 0.1.x aliases of shard-pair count; no dual-select runtime
  exists.
- P1 Delta mixed follow-up: hierarchical directory-chunk COW (plus O(1) arena key counters / in-place
  store) to cut per-publication spine traffic without changing version capacity or QSBR lifetime
  ([paired-delta-directory-chunks-2026-07-31](docs/benchmarks/paired-delta-directory-chunks-2026-07-31.md)).
  macOS advisory A/B is noisy; Linux hard-pinned confirmation still required for magnitude claims.
- P1 reject get-into / multi-extent scatter promotion without bounded+win proof
  ([paired-get-into-multi-extent-reject-2026-07-31](docs/benchmarks/paired-get-into-multi-extent-reject-2026-07-31.md)).
- P1 Linux hard-pinned 1/2/4/8 harness + runbook + `workflow_dispatch` self-hosted workflow
  ([paired-shards-linux-p1](docs/benchmarks/paired-shards-linux-p1.md)); macOS evidence stays advisory.
- Add CI assert for typed SDK backup helpers (`scripts/assert-sdk-backup-helpers.sh`): fail closed
  if any official SDK (C++/Python/Go/Perl/Ruby/Erlang) lacks a typed `backup`/`Backup` surface for
  wire `BACKUP`; wired into `sdk-clients`. Runtime interop smoke:
  `scripts/test-sdk-backup-interop.sh`.
- Add SDK artifact attestation verification gate
  (`scripts/verify-sdk-artifact-attestations.sh`): fail-closed `gh attestation verify` on
  tagged supply-chain runs when attestations are produced (public or
  `ENABLE_ARTIFACT_ATTESTATIONS`); soft-skip otherwise. Residuals: project GPG, full SLSA L3,
  non-GHEC private without the opt-in variable.
- Expose typed `backup(destination)` on official SDKs (Python, Go, Perl, Ruby, Erlang) mirroring
  C++ `Client::backup` for wire `BACKUP` (opcode 10): worker-0 routing, ASCII report on success,
  fenced (not hot zero-impact) semantics; admin under secure authz.
- Expose typed C++ `Client::backup(destination)` for wire `BACKUP` (opcode 10): worker-0 routing,
  fenced (not hot zero-impact) semantics.
- Expose online fenced backup on the live daemon: wire opcode `BACKUP` (10) and `Server::backup_to`,
  admin-gated under secure authz.
- Add online durable backup via `Store::backup_to`: fence admissions, flush, copy catalog under the
  open Store lock (writer fence, not fully hot concurrent I/O). Offline CLI still requires a stopped
  Store. Docs/CHANGELOG honesty updated; concurrent-writer unit coverage included.
- Harden E3 rehearsal (not certification): `scripts/assert-e3-rehearsal-honesty.sh` fails closed on
  accidental `e3_certified=yes` / release-eligible labels; campaign `--e3-profile`; weekly
  `durability-evidence.yml` runs campaign-profile loopback/APFS + hosted-ci E0→E3 orchestrator
  rehearsal while keeping `e3_certified=no`.
- Extend secure-profile interop with Phase 5 principal quota → wire `OVERLOADED` (single-connection
  burst) and refresh docs that still claimed the secure matrix was incomplete.
- Add cross-builder SDK archive digest compare (`scripts/compare-sdk-artifact-sums.sh`, job
  `sdk-repro-cross` on `ubuntu-22.04` vs primary supply-chain sums) for tags/dispatch/weekly.
- Normalize Python sdist / Perl tar.gz with `scripts/normalize-tar-gz.sh` (epoch mtimes +
  `gzip -n`) so two-pass reproducibility covers wheels, gems, and those archives.
- Pin `SOURCE_DATE_EPOCH` for SDK packaging (`scripts/export-reproducible-build-env.sh`) and gate
  two-pass wheel/gem digest equality (`scripts/verify-sdk-artifact-reproducibility.sh`) on tags /
  workflow_dispatch. Residual: normalize Python sdist / Perl tar.gz host metadata.
- Expand secure-profile interop to ruby/erlang when toolchains are present (CI already has
  Ruby 3.3 + Erlang/rebar3; local soft-skips). Full official-SDK secure matrix is now opt-in by
  availability rather than cpp/python/go-only.
- Add GitHub SLSA provenance attestations for tagged SDK packages (`actions/attest` in
  `supply-chain.yml`; public repos or private with `ENABLE_ARTIFACT_ATTESTATIONS=true` / GHEC).
- Include Perl in secure-profile interop when `IO::Socket::SSL` is present (CI installs
  `libio-socket-ssl-perl`). Residual: ruby/erlang in that matrix.
- Wire Cosign/Sigstore keyless `sign-blob` for packaged SDK artifacts on tag pushes
  (`.github/workflows/supply-chain.yml`); verify bundles in-job. Project GPG remains optional.
- Add gitleaks + Trivy filesystem scanning CI (`.github/workflows/supply-chain-scan.yml`) for
  Phase 7.1 secret/dependency gates on main and PRs.
- Expand secure-profile interop smoke (`scripts/test-secure-profile-interop.sh`): authz deny for
  unmapped mTLS principals, `prefix=` key-scope allow/deny, and `--tls-crl` rejection of revoked
  client certs (cpp/python/go happy path unchanged). Residual: perl/ruby/erlang in that matrix.
- Add first-slice secure-profile interop smoke (`scripts/test-secure-profile-interop.sh`): mTLS
  client/server PEMs, `--authz-map` write principal, pinned `--worker-hash-seed` under
  `--secure-profile`, cpp/python/go PUT→GET + keyed owner checks; wired into CI `sdk-clients`
  (`timeout-minutes: 5`, TLS build forced `GLIFISTORE_ENABLE_TLS=ON`).
- Add supply-chain CI gate (`.github/workflows/supply-chain.yml`): package SDKs, require `syft`
  SPDX JSON (`SYFT_REQUIRED=1`), upload `SHA256SUMS` + `*.spdx.json`. Tag Cosign keyless signing
  and GitHub SLSA attestations land on tags (public / `ENABLE_ARTIFACT_ATTESTATIONS`); verify
  via `scripts/verify-sdk-artifact-attestations.sh`. Residual: project GPG / full SLSA L3.
- Complete the ADR 0030 keyed Worker routing SDK train: Python / Perl / Go / Erlang / Ruby decode
  plain and extended INIT identities, implement SipHash-2-4 bit-for-bit with C++, and route with
  the disclosed seed. Cleartext FNV default path unchanged. Interop harness runs FNV for the full
  worker list and, when `INTEROP_KEYED=1` (default), a SipHash cleartext matrix for workers 2/4.
- Add Phase 8 Unix-domain socket transport with optional peer-credential principals
  ([ADR 0029](docs/adr/0029-uds-peercred.md)): `--unix-socket PATH`, `--unix-peercred` →
  `unix:uid=N` for `--authz-map`; `--secure-profile` requires peercred when UDS is enabled.
  Linux `SO_PEERCRED`, macOS/FreeBSD/OpenBSD `getpeereid`. Not a TLS replacement.
- Add Phase 8 keyed Worker routing ([ADR 0030](docs/adr/0030-keyed-worker-routing.md)):
  `--worker-hash-seed`; Manifest-persisted seed; INIT identity extension; C++ client train;
  `--secure-profile` randomizes unless pinned. Default Stores stay FNV-1a. Not full multi-tenant.
- Add Phase 8 hash-flood + STATS isolation slices (not a full multi-tenant product):
  - Keyed Index mix seed ([ADR 0026](docs/adr/0026-keyed-index-hash-seed.md)):
    `--index-hash-seed`; `--secure-profile` randomizes unless pinned.
  - Prefix-scoped principals need `admin` for daemon-wide `STATS`
    ([ADR 0027](docs/adr/0027-stats-isolation-prefix-principals.md)).
  - Per-tenant data-dir deferred honestly
    ([ADR 0028](docs/adr/0028-per-tenant-data-dir-deferred.md)).
- Add Phase 8 first-slice key-prefix authz scope (`prefix=` in `--authz-map`, ADR 0025): deny
  cross-prefix `GET`/`PUT`/`ERASE` with wire `PERMISSION_DENIED`; omit prefix for whole-keyspace
  principals. Document residual risks (shared data dir; keyed routing now closed by ADR 0030 SDK
  train) — not a full multi-tenant product.
- Implement Phase 6 security audit + CRL fail-closed: JSON-lines `auth`/`authz`/`tls` events
  (`SecurityAudit`), `STATS` auth/tls/authz counters, `--tls-crl` / `--tls-ocsp-fail-closed`
  (CRL required; live AIA OCSP HTTP unsupported), OpenBSD unveil of CRL paths, and
  [secure-profile-certs.md](docs/operations/secure-profile-certs.md). Residual public-bind blockers:
  operator CRL config, multi-tenant Phase 8 remainder, physical E3.
- Add operator-ready E3 campaign package: `scripts/run-e3-campaign.sh` orchestrates E0→E1→E2→E3
  (many reps, evidence tarball + SHA-256 manifest) while keeping `e3_certified=no`; document pin,
  PASS/FAIL/INCONCLUSIVE, artifact layout, and human promotion gate in
  `docs/operations/e3-campaign.md` and `platform-durability-evidence.md`. No row is E3 certified.
- Add multi-hour soak profiles (`smoke`/`long`/`1h`/`4h`) to `scripts/soak_daemon.sh` with optional
  RSS and STATS sampling (rotation/compaction counters); document honesty in
  `docs/operations/soak.md` and production-readiness. Wire optional CI via
  `.github/workflows/soak-extended.yml` (`workflow_dispatch` + monthly 1h only — not every PR).
- Implement OpenBSD Phase 6.5 `pledge`/`unveil` confinement after `Server::create` (data dir +
  TLS/authz paths; fail closed; no-op on Linux/macOS/FreeBSD). Cover promise-set unit tests and
  OpenBSD CI grep for `openbsd-sandbox=pledge+unveil`.

- Implement Secure profile Phase 5 abuse / DoS controls: process-wide `--max-accepts-per-sec`,
  `--idle-timeout-ms` / `--request-timeout-ms`, per-connection and per-principal request/bandwidth
  quotas, shared `AbuseController`, `STATS` `abuse_*` counters, and `--secure-profile` defaults that
  refuse explicit `0`. Trusted cleartext keeps limits disabled unless set.
- Close Ruby SDK TLS honesty gap: opt-in TLS 1.3 (`ClientConfig#tls`, CA / mTLS / hostname verify,
  lab `insecure_skip_verify`) matching Go/Python/Perl; include Ruby in the TLS interop matrix;
  retire the “Ruby cleartext exception” from the security same-train docs.
- Wire tagged release-artifact compatibility into CI (`released-artifact-compat` job in
  `.github/workflows/release-compat.yml`): decode in-tree `tests/fixtures/released/` (including
  `self-v1`), package a per-SHA self artifact on push/PR, and on tags package + upload
  `released/<label>/`. Register `released_artifact_compat_tests` in the unit test binary.
- Advance E3/E4 durability certification scaffolding: `scripts/run-e3-block-reset.sh` provisions
  disposable linux-ext4 (loopback + optional dm-flakey) and macOS APFS (hdiutil) rows, arms abrupt
  block-device reset at crash-harness checkpoints, remounts with non-repairing fsck, and records
  honest `e3_certified=no` artifacts; expose `glifistore_crash_persistence --mode seed`; add
  `.github/workflows/durability-evidence.yml` for E2 collector + E3 harness smoke; document PASS/FAIL
  and promotion rules in `platform-durability-evidence.md`. No filesystem row is E3/E4 certified.
- Advance durable hot-cache probing to Swiss-style H2 control bytes with SIMD/scalar 8-slot group
  matching (shared `swiss_control_group.hpp`), and keep full-key identity checks. Defer catalog
  shared-lock acquisition in `prepare_get` / cold revalidation off the ordinary hot path (Worker
  mutex only until a cold miss needs a generation pin). Add `hot_record_table` unit coverage and
  record comparative GET medians in `docs/benchmarks/get-path-hot-cache-simd-2026-07-25.md`.
- Finish durable GET path follow-up optimizations: slim `prepare_get` critical section (hot snapshot
  then unlock; pin only on cold miss; deferred TTL drain only when backlog non-empty), move hot-cache
  bookkeeping to cache-line-aligned relaxed atomics, gate fine-grained GET timing out of Release
  builds (`NDEBUG`, overridable with `GLIFISTORE_GET_PATH_TIMING`), and replace the hot map with a
  flat open-addressed table (FNV hash, load 0.5, 48-byte inline values, in-place staging). Raise
  default GET-path bench ops. The prior −20% v32 regression is closed at credible op counts.
- Record durable GET path + hot-cache optimization notes (comparative microbenchmarks, sanitizer
  notes, preserved invariants, and discarded alternatives) without retaining run artifacts in-tree.
- Tighten durable hot-cache structure: max_load_factor 0.5, geometric reserve, 32-byte inline
  values to avoid heap allocations on small payloads, and documented per-entry accounting. Hash is
  never treated as identity; full key compare remains mandatory on collision.
- Add explicit durable hot-cache controls: `hot_cache_enabled`, `max_hot_cache_value_bytes`
  (default 64KiB), daemon `--disable-hot-cache` / `--max-hot-cache-value-bytes`, and stats for
  hit-rate, size-rejected, enabled, and max value. Oversized values never admit; disabling the
  cache leaves cold pinned reads correct.
- Defer durable Index TTL reclaim to a bounded per-Worker backlog drained by existing Worker paths
  (`prepare_get`, `mutate`). Expired GETs still return `not_found` immediately, drop hot rows, and
  never serve expired values; reclaim verifies exact `RecordRef` before erase so reinserts survive.
- Resolve durable GET generation pins in O(1) via a dense `SegmentId` → catalog-slot side table
  rebuilt on recovery, rotation, and compaction. Index↔catalog identity, generation, owner, and
  pin-object checks remain mandatory; `RecordRef` identity is unchanged.
- Add low-overhead durable GET path telemetry (`get_path_stats()`): Worker mutex wait, prepare/complete
  lock hold, Index/hot-cache/generation-pin lookup time, cold read and CRC/value-copy time,
  relinearization retries, hot-cache hit/miss/stale/eviction, expired-TTL GETs, and hot-cache resident
  bytes/entries. Extend `hot_cache_stats()` with stale, eviction, size-rejected, and expired counters.
  Behavioral GET/hot-cache optimizations follow in later commits; capture baselines first.
- Wire the consolidation slice so documented secure-profile / migrate / STATS surfaces match the
  binary: CMake builds `authz.cpp`, `store_migrate.cpp`, `glifistore_migrate_store`, and the orphan
  unit tests; mTLS extracts principal (URI SAN → DNS SAN → CN); the reactor enforces
  `--authz-map` with wire `PERMISSION_DENIED` (8); `--secure-profile` fails closed (no dual
  `--tls-port`); durable lanes export `LatencyHistogram` needles plus
  `maintenance_rate_window_*` in `STATS`; daemon CLI exposes
  `--maintenance-max-copy-bytes-per-sec` / `--maintenance-max-cpu-ms-per-window` and extends
  `--dump-config`. Official SDKs (including Ruby) share the TLS “same train” policy for opt-in
  TLS 1.3 connect options.
- Complete durable TCP daemon software path (P0-01). Extend `--dump-config` with maintenance
  thresholds, durable resource defaults, disk-read settings, and group batch fields. Extend
  `glifistore_crash_daemon` with pre-commit PUT and post-ack ERASE checkpoints. Mark P0-01
  software-complete; E3/E4 power-loss remain open (histogram export now wired into STATS).
- Add end-to-end operator guide for durable `glifistored` deployments
  (`docs/operations/durable-tcp-daemon.md`): profile or explicit storage mode, data directory and
  open policy, resource/batch/maintenance flags, `HEALTH`/`READY`/`STATS` expectations, shutdown
  drain, offline backup/verify/repair pointers, and explicit unsupported claims (live backup,
  power-loss certification pending). Link from operations index, CLI reference, production readiness,
  and v1 production roadmap.
- Close durable unread-TTL normal-mode policy fail-closed. Default scheduling stays conservative
  (Index-referenced dead bytes only). Opt-in `unread_ttl_normal_scheduling` probes unread expired
  sealed puts during normal evaluations and adds them to `candidate_scheduling_dead_byte_ratio_bp`
  for the inclusive dead-byte threshold only; compaction still uses the sole `Store::compact()`
  path and copy budget still uses exact live bytes. Export scheduling ratio through
  `MaintenanceSnapshot` and daemon `STATS`. Add daemon CLI/config for unread-TTL probe and normal
  scheduling flags.
- Document permanent refusal of `glifistore_rebuild_index` for durable v1 with explicit operator
  paths via Store recovery and `glifistore_repair_store`. Sync persistence roadmap: software P0-08
  policy slices closed; controlled native baselines and E3/E4 power-loss certification remain open.
- Add fail-closed JSON-lines structured logging for `glifistored` lifecycle events (`start`, `listen`,
  `ready`, `maintenance_emergency`, `maintenance_fault`, `shutdown_begin`, `shutdown_drain_begin`,
  `shutdown_drain_end`, `stopped`, `executor_failure`). Opt in with `--log-format json` (default
  `human` keeps legacy stdout/stderr). Structured fields are bounded, omit secrets, and `--quiet`
  suppresses only the normal startup/shutdown lifecycle lines.
- Add operator runbooks under `docs/operations/` for graceful drain and overload (`HEALTH`/`READY`/`STATS`,
  `--shutdown-drain-ms`), offline backup/restore (`glifistore_backup_store`, `glifistore_verify_store`),
  and corruption detection/repair (`glifistore_verify_store`, `glifistore_inspect_segment`,
  `glifistore_repair_store` with quarantine outside the live store). Link from the documentation index,
  production readiness, persistence roadmap, and architecture backup-restore guide.
- Add fail-closed unread-TTL observability for pressure/emergency maintenance. When
  `unread_ttl_pressure_probe` is enabled (default), background evaluations under segment or
  free-space pressure, or emergency, perform a bounded sealed-Index probe of the round-robin
  candidate and export unread expired sealed Record counts/bytes through
  `MaintenanceObservation`, `MaintenanceSnapshot`, and daemon `STATS`. Normal policy remains
  Index-referenced dead bytes until pressure or an explicit `Store::compact()` visit. Fail-closed
  probe faults disable auto-compact unless an emergency gate is already armed.
- Add a deterministic whole-Worker durable compaction planner, generation-safe v1 manifest
  replacement, physical temporary/peak/amplification gates, a checksummed dual-manifest intent
  codec, descriptor-relative intent publication/removal, and restart resolution against exactly
  the old or next authority with validated, idempotent Segment retirement. Add a durable builder
  that prebuilds the new Index, preserves visible v1 Record bytes and sequences, reclaims
  expired/superseded/tombstoned history, and validates sealed replacements. Runtime installation
  publishes the prepared manifest, commit catalog, and Worker Index atomically and retires sources.
  Public `Store::compact()` selects Workers round-robin without a background thread, skips exact
  no-gain layouts, executes at most one transaction per call, and returns copy statistics.
- Add fail-closed daemon deployment profiles (`dev`, `embedded`, `production`) with precedence
  `defaults < profile < file < env < CLI`. Unknown profile names fail before listen; `--dump-config`
  prints the selected profile plus resolved settings.
- Normatively specify duplicate `request_id` and reconnect semantics for wire protocol v2. `request_id`
  is correlation-only with no server deduplication; transport loss requires re-`INIT`/`BIND_WORKER`;
  mutations with bytes sent remain indeterminate until application reconciliation. Update
  [wire protocol v2 §8.1 and §10.1](docs/spec/wire-protocol-v2.md), [client semantics v1 §5](docs/spec/client-semantics-v1.md),
  ADR 0019, and close the corresponding production-roadmap bullets.
- Expose durable no-gain planning work that previously looked identical to cheap scheduler skips.
  `Store::compact()` still returns `compacted == false` without publishing an intent, but now reports
  the Worker examined plus exact Index-referenced sealed Record/byte counts verified before the
  layout rejected the rewrite. `MaintenanceSnapshot` and daemon `STATS` accumulate last/total no-gain
  scan counters alongside `skips`, `consecutive_no_gain`, and `last_skip_reason`.
- Replace fail-fast unrelated-Worker rotation during a durable compaction lease with condition-based
  serialization: the rotation now waits, rebuilds from the newly published Manifest, commits, and
  survives reopen without changing persistence v1. Advance the automatic round-robin cursor for
  every observed candidate so a below-threshold Worker cannot starve reclaimable peers, and expose
  a cumulative maintenance sequence-conflict counter through snapshots and daemon `STATS`. Extend
  the maintenance benchmark with forced-rotation, idle, and sustained-churn scenarios. On the clean
  seven-repeat macOS/APFS follow-up, all forced rotations commit without foreground errors; the
  serialized boundary costs about 2.5x median latency, product-default idle duty is about 0.0018%,
  and seven 1 GiB churn samples finish with four Segments instead of 22 at a 2.9% median throughput
  cost. Add atomic runtime rotation telemetry for attempts, commits, compaction waits, and
  last/total/maximum publication-wait, execution, and total durations; surface it through
  `MaintenanceSnapshot`, daemon `STATS`, and phase-aware maintenance CSV. The clean macOS matrix
  attributes 71--75% of forced-overlap latency to publication wait; under 1 GiB churn only one of
  16 rotations waits, all commit, and background maintenance retains four versus 22 Segments at a
  2.5% median throughput cost. Split execution telemetry into Segment seal, replacement Segment
  creation, Manifest publication, residual in-memory execution, and the post-rotation final Record
  commit. Serialize only the short multi-writer atomic statistics update, without adding a storage
  lock or extending the compaction publication lease. The clean macOS deep-phase matrix attributes
  65--72% of forced rotation execution to replacement creation and about 0.3 ms to final Record
  commit. Require the churn harness to observe a complete quiescent Worker sweep before settling;
  all corrected samples compact to four instead of 22 Segments.
- Add a reproducible concurrent-maintenance benchmark comparing disabled, cooperative, and
  Store-owned background policy under a synchronized mixed GET/PUT workload. Record a clean
  seven-repeat macOS/APFS matrix with raw CSV: both maintenance modes complete the same 31.01 MiB
  useful compaction without conflict, while median foreground throughput falls about 18% and p99
  rises 54--57% versus disabled. Cooperative and background medians are effectively equal. A
  rotation-forcing calibration exposed unrelated-Worker fail-fast publication conflict; the follow-up
  above closes and measures that path.
- Enforce `dead_byte_ratio_bp_normal` for normal durable maintenance using exact per-Worker
  Index-referenced active/sealed Record-byte counters maintained across recovery, mutation, lazy
  expiry, rotation, compaction, and reopen. Observe the next round-robin candidate without scanning
  its Index, pass that exact Worker to automatic compaction, retain pressure/emergency threshold
  bypass, and export candidate sealed/live/dead bytes plus basis-point ratio through
  `MaintenanceSnapshot` and daemon `STATS`.
- Make `max_copy_bytes_per_cycle` a preventive per-candidate limit for normal durable background
  maintenance instead of an after-the-fact accumulated counter. Set a finite 128 MiB default
  derived from the first compaction matrix, allow equality, define zero as explicitly unlimited,
  retain pressure/emergency bypass, report a distinct `copy_budget` policy reason, and expose the
  setting through daemon CLI/config/environment plus `--dump-config`.
- Add a dedicated public `Store::compact()` benchmark with high/medium/low reclaim, copy-heavy,
  50% TTL, and no-gain workloads. Each fresh-Store sample closes, reopens, verifies the Index, and
  checks its complete key model; CSV output includes segment/byte reclamation, copied and expired
  records, elapsed time, and effective scan/copy rates. Record the first seven-repeat exploratory
  macOS/APFS result and use it to drive the per-Worker dead-byte enforcement and finite normal copy
  limit above. No-gain planning scans now expose public counters through `CompactionResult`,
  `MaintenanceSnapshot`, and daemon `STATS`. Unread TTL remains conservative under normal
  policy, and concurrent foreground cost is measured by the dedicated follow-up matrix.
- Add the platform durability evidence matrix with cumulative E0–E4 claim levels, APFS/Linux/BSD
  row inventory, artifact/promotion requirements, and a controlled power-loss campaign protocol.
  Add a portable collector that records source, OS, hardware class, filesystem/mount, toolchain,
  commands, results, and SHA-256 provenance while limiting itself to metadata or process-kill
  evidence.
- Extend compaction recovery beyond the single-output fixture: interrupt rollback between two
  replacement unlinks and roll-forward between three source unlinks, then prove that an ordinary
  reopen preserves the selected manifest authority and completes every remaining cleanup. Add the
  same two-output cleanup transitions to the SIGKILL harness. Exercise the online multi-output path
  end to end with 64
  maximum-size live Records compacted from three sources into two replacements and reopened. Add
  four reproducible model histories covering 608 total PUT/ERASE/TTL operations; each reports its
  seed and must agree before compaction, after installation, and after reopen. Add five faulted
  seeds covering another 760 operations across intent, Record-copy, Manifest-sync, source-retirement,
  and intent-removal failures, with exact outcome/health/authority and reopen-model checks. Replace
  the minimal online crash seed with a deterministic 30-operation PUT/ERASE/TTL history and verify
  its complete eight-key model after every one of the 25 compaction SIGKILL checkpoints. Add 15
  differential online 3-to-2 SIGKILL checkpoints for second-replacement creation, its final Record
  and data/seal commits, shifted Manifest/retirement directory syncs, and the third source unlink,
  bringing the sync matrix to 91 occurrence-specific checkpoints. Make rollback remove both the
  canonical and partial temporary name for every obsolete replacement identity so a crash during
  second-output creation reopens with the old authority and a clean namespace. Batch the immutable
  source seed at Segment seal and add an opt-in `copy-matrix` covering the remaining 63
  `write_record` occurrences; together the standard and exhaustive profiles kill after all 154
  distinct checkpoints, including every one of the 64 maximum-size Record copies. Add an opt-in
  `random-matrix` with four reproducible 96-operation PUT/overwrite/ERASE/TTL histories. Across 36
  process kills it checks nine old/next-authority checkpoint classes per seed and verifies the
  complete 64-value maximum-size model after recovery.
- Add the normative persistence-v1 recovery state-transition matrix covering bootstrap,
  mutation/flush, rotation, compaction, ordinary rejection rules, and the exact automated evidence
  attached to each restart outcome. Make repeated bootstrap/rotation directory and commit
  checkpoints occurrence-specific, and verify the post-rotation mutation's absent/optional/present
  recovery boundary instead of only preserving the seed key.
- Add the independently generated Compaction Intent v1 golden fixture, bind the production encoder
  and decoder to its canonical dual-Manifest transition, and close the final persistent-codec
  fixture gap in the compatibility and readiness matrices.
- Fix daemon CLI option dispatch so `--workers` and `--max-connections` retain their distinct
  values, and make the SDK interoperability matrix verify the server's effective configuration.
  Expand that matrix through 8 Workers with deterministic owner coverage, structured `NOT_FOUND`,
  and uniform local 2 MiB frame-limit rejection across C++, Python, Perl, Go, and Ruby. Stabilize
  background-maintenance tests under sanitizers, restore the strict Perl quality gate, and document
  the completed cross-SDK wire-v2 golden-fixture coverage.
- Add an installable synchronous C++ wire-v2 client with per-Worker bound connections, explicit
  committed/rejected/indeterminate mutation outcomes, canonical independent wire fixtures, and a
  public-client TCP benchmark mode. Split the public filesystem fault-hook types from persistence
  implementation headers so installed Store and client consumers compile against a closed header set.
- Fix hardening feature checks to evaluate their result variables, enforce extension-free ISO C++23
  while permitting newer standards, apply supported stack-protector and PIE flags to every binary,
  and enable optimized Linux `_FORTIFY_SOURCE=3`, RELRO, and immediate binding. A strict Release CI
  job verifies both emitted compile/link commands and the resulting ELF security properties.
- Track SwissTable deleted controls and effective occupancy, reuse tombstones exactly, and perform
  bounded same-capacity cleanup before probe stability degrades. Rehash now builds an independent
  table and key arena before atomic in-memory installation, preserving the old Index on allocation
  failure; expose tombstone, effective-load, probe, rehash, and cleanup statistics plus a churn-miss
  benchmark.
- Bound the durable active-Record hot cache with deterministic global/per-Worker byte partitions,
  entry and staging limits, conservative observable accounting, and cold fallback on exhaustion.
  Hot GET now snapshots shared immutable value ownership and copies outside the Worker mutex; active
  misses use an exact generation pin plus post-I/O Index/pin revalidation. Add coverage for value
  sizes through near 1 MiB, zero/minimum budgets, overwrite, erase, TTL, rotation, and strict group
  publication.
- Move durable cold GET file I/O and CRC off Worker-affine network Reactors through a bounded shared
  executor. Prepared jobs own the exact `RecordRef` and immutable generation pin, completions return
  through bounded Reactor queues with `(slot, generation)` rejection and relinearization, saturation
  returns `overloaded`, per-connection pipeline order is preserved, and shutdown cancels queued work
  before draining in-flight reads. Add deterministic TCP coverage for blocked same-Worker progress,
  saturation, close/slot reuse, stale completion, and shutdown drain.
- Make volatile long-key erase reclaim geometrically instead of recopying the live key arena at
  every fixed 64 KiB of churn. Add strict-group record-target adaptation bounded by explicit
  `min_records`/`max_records`, contracting on deadline occupancy and growing with admitted producer
  pressure without changing acknowledgement semantics.
- Add per-directory deterministic file-I/O fault injection for short positional transfers,
  `EINTR`, writeback `EIO`, disk/quota exhaustion, and read-only filesystems; extend pre/post
  publication and mutation recovery-oracle matrices without global test state.
- Add validated durable resource limits for Store bytes, reserved free space, Segment and manifest
  counts/bytes, open descriptors, recovery memory, live keys, temporary compaction space, and write
  amplification; preflight bootstrap and rotation before publication/sealing and expose stable
  storage, file-size, and descriptor exhaustion errors.
- Add idempotent public `Store::close()` with atomic admission quiescing, in-flight operation
  draining, forced partial group closure, observable sticky final-flush errors, concurrent
  flush/close safety, cache-line-isolated per-Worker accounting, executor shutdown, and immediate
  resource/data-directory lock release.
- Make ordinary Store reads and durable recovery share a checked Unix-epoch clock, add thread-safe
  `StoreClock` injection, clamp backward movement per Store instance, and remove public per-read
  timestamp overrides that silently disabled TTL expiration by default.
- Preallocate hot-cache publication nodes before persistent writes, translate public/background
  exceptions into stable failures with fail-closed waiter release, and reject overlapping or
  reversed per-Worker sequence ranges across persistence v1 Segments.
- Add an isolated deterministic allocation-fault executable that exhaustively fails each observed
  durable put, update, erase, read, group-commit, and rotation allocation; verify recovery outcomes,
  prohibit steady-state allocation after the Record write boundary, and exercise background waiter
  release on `bad_alloc`.
- Add strict `durable_group` batching with whole-batch publication, absolute batch deadlines, a
  dedicated one-Worker v1 commit executor, bounded threshold admission, latency benchmarks,
  crash/rotation coverage, and macOS `F_BARRIERFSYNC` ordering before the final full durable flush.
- Enable public `Store::open(durable_sync)` with explicit create/open policies, persisted Worker-count
  validation, entropy-backed Store IDs, crash-recoverable bootstrap intents, and the durable
  mutation/rotation runtime behind the PImpl.
- Define the target alpha durability, recovery, routing persistence, and public read ownership
  contracts in ADRs 0008 and 0009.
- Separate the installed C++ API from engine internals with a PImpl Store, owning reads, byte-key
  overloads, and a build-tree-only server/test access bridge.
- Rename Store read benchmark result identifiers with a `_copy` suffix so owning-read measurements
  cannot be compared accidentally with the former non-owning prototype baseline.
- Add the exact little-endian Segment header v1 codec, alternating CRC32C commit-slot selection,
  fail-closed corruption/version handling, and a canonical golden fixture.
- Add the canonical manifest v1 codec with full-file CRC32C, bounded decoding, deterministic Segment
  catalog invariants, publication-generation selection, and a golden fixture.
- Add a canonical binary-key Record v1 fixture, require minimal aligned extents and zero padding on
  decode, centralize hexadecimal fixture loading, and document the format compatibility matrix.
- Add platform-aware POSIX persistence primitives with descriptor-anchored private paths, complete
  positional I/O, exclusive directory locking, strict synchronization, atomic manifest replacement,
  explicit indeterminate outcomes, and filesystem fault-injection tests.
- Add exact-size durable Segment files with platform-specific physical allocation, verified identity
  reopening, Record-before-slot synchronization, alternating commit/seal slots, bounded committed
  recovery scans, and explicit not-committed versus indeterminate fault outcomes.
- Add manifest-driven durable recovery with constant Segment-descriptor usage, single-pass committed
  Record visitation, per-Worker bounded latest-key rebuild, persisted hash/routing validation,
  sequence restoration, and interrupted-rotation detection.
- Add bounded descriptor-relative namespace audit with strict canonical Segment parsing,
  deterministic anomaly reports, crash-temporary tolerance, no-follow object checks, and fail-closed
  rejection of unlisted, malformed, unknown, unsafe, or missing entries before recovery scans.
- Add read-only durable runtime materialization with moved recovered Indexes, one cached Segment
  descriptor per Worker, read-only/no-follow reopening, per-read CRC/key/reference verification,
  sticky fail-closed corruption handling, and interrupted-rotation service refusal.
- Add the internal durable mutation state machine with preflighted allocation-free Index publication,
  ordered Record and commit-slot synchronization, explicit mutation outcomes, shared catalog
  concurrency, and exact-intent crash-safe Segment rotation completion.
- Add OpenBSD as a `kqueue` architectural target from the `0.1.0` prototype line.
- Bootstrap the C++23 architecture prototype.
- Add fixed 64 MiB append-only segments, explicit record codec, derived Index, rebuild, vacuum,
  worker sizing, tests, fuzz targets, benchmarks, and macOS/Xcode tooling.
