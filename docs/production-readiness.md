<!-- GENERATED FILE. Do not edit by hand.
     Authority: engineering/gates/*.yaml
     Regenerate: python3 engineering/tools/validate_assurance.py --write-generated
-->

# Production readiness

> **Derived view.** Machine-readable authority lives under
> [`engineering/gates/`](../engineering/gates/). GlifiStore remains an
> **architectural prototype**. A release level advances only when every
> mandatory gate below has automated evidence. A design document or
> implementation alone does not close a gate.

## Daemon runtime boundary (0.1.0)

`glifistored` runs only the paired Reader–Writer model
([ADR paired shards](adr/paired-reader-writer-shards.md),
[server model](architecture/server-model.md)): one ShardPair (Reader + serial
Writer + SPSC lanes) per owner id. There is no dual-select daemon runtime.
The volatile engine under `src/experimental/` is lab-only.

## Release levels

- **Prototype:** architecture and performance exploration; no compatibility or durability promise.
- **Alpha:** public API and formats are versioned; destructive changes remain possible.
- **Beta:** durability, recovery, upgrade, security, and operational contracts are feature-complete.
- **Release candidate:** only correctness, compatibility, security, and operability fixes are accepted.
- **Stable:** supported upgrade paths, published artifacts, and an explicit support lifetime exist.

## Mandatory gates

| State | Meaning |
| --- | --- |
| `NON_INIZIATA` … `IMPLEMENTATA` | Work incomplete or not yet proven |
| `PROVATA_LOCALMENTE` | Proven outside mandatory CI evidence |
| `PROVATA_IN_CI` | CI evidence path exists and is linked |
| `PROVATA_SU_HARDWARE` / `VERIFICATA_INDIPENDENTEMENTE` / `ACCETTATA_PER_RILASCIO` | Higher claim levels |

### Public contract

- [x] **GATE-API-ABI-POLICY** — API and ABI compatibility policies for patch/minor/major
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-COMPAT-FIXTURE-001`, `GS-COMPAT-NN1-001`, `GS-COMPAT-CABI-001`
  Residual risk: Permanent tagged N−1 fixtures and the first old-binary/new-library C ABI row remain release-process steps
  C++ ABI remains not promised; C ABI v1 has an independent contract and CI-linked gate, while tagged cross-release proof remains residual.

- [x] **GATE-C-ABI-V1** — C ABI v1 layout symbols lifecycle and installed consumption
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-COMPAT-CABI-001`
  Residual risk: No tagged ABI-1 old consumer fixture and no retained native BSD package evidence yet
  Pure C runtime, negative symbol gate, layout assertions, SONAME authority, CMake target and installed consumer are proven in ci.yml install-consumer; cross-release proof remains open.

- [x] **GATE-CONCURRENCY-SPEC** — Error behavior, limits, time, and concurrency guarantees specified
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-CONCUR-PAIR-001`, `GS-CONCUR-COMBINE-001`, `GS-CONCUR-LIN-001`, `GS-CONCUR-FAULT-001`, `GS-CONCUR-MEM-001`, `GS-CONCUR-TLA-001`, `GS-CONCUR-LIVE-001`, `GS-CONCUR-LEGACY-001`, `GS-PROTO-WIRE-001`, `GS-PROTO-ERROR-001`, `GS-CORE-CLOSE-001`
  Residual risk: Formal model bounds and declared fairness do not prove real scheduler fairness; checker history size bounded; admitted Store mutations remain non-cancellable by disconnect/timeout (by design); durable_group/periodic flusher token sharing deferred; adversarial multi-hour combiner fairness evidence open
  Client semantics, error taxonomy, concurrency model (including ADR 0037 token combining Phases A–C), B1 checker/hooks/TLA+, legacy_mutex policy, and daemon request/idle timeout (no cancel of admitted Store work) are normative. Phase C daemon mutation windows (≤32) and GET visibility barrier are proven by mutation_window_tests plus server_reactor_mutation_window_tests (contiguous PUT window/resume, PUT→ERASE→GET, durable_sync GET/PING barriers).

- [x] **GATE-DISK-WIRE-VERSIONS** — Disk and wire formats versioned with fixtures and matrices
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-COMPAT-FIXTURE-001`, `GS-COMPAT-NN1-001`, `GS-PROTO-WIRE-001`
  Residual risk: Publishing trees into permanent fixture drop remains a release-process step
  Golden fixtures and released-artifact harness exist; permanent drop process open.

- [x] **GATE-PUBLIC-API-OWNERSHIP** — Supported API separated with ownership/lifetime model
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-CORE-API-001`
  Residual risk: No ABI stability before 1.0
  The installed API uses owning reads; internal access remains build-tree-only.

### Durability and recovery

- [x] **GATE-BACKUP-RESTORE** — Backup restore verification and version migration
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-OPS-BACKUP-001`, `GS-OPS-MIGRATE-001`
  Residual risk: Released-tag artifact consumption remains open; zero-fence hot backup out of scope; E3/E4 open; glifistored mid-BACKUP kill covered by glifistore_crash_backup_daemon
  Offline tools and online fenced backup with ENOSPC and concurrent fence proofs in CI; normative snapshot boundary published. Not E3/E4 certified.

- [x] **GATE-DURABLE-ACK** — Acknowledgement semantics for durable mutations
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-PERSIST-ACK-001`
  Residual risk: No native filesystem row has E3/E4 certification; ADR 0037 durable_sync combiner shares ACK-after-visibility with the dedicated Writer path
  E2 evidence present; physical E3 honesty enforced by rehearsal scripts. Embedded durable_sync may combine under the execution token (ADR 0037 Phase B) without changing ACK-after-visibility polarity. Durable erase_batch tombstones across reopen are covered by paired_store_erase_batch_durable_litmus_tests for embedded and Writer-path durable_sync / durable_periodic, and Writer-path durable_group (no claim-ceiling change).

- [x] **GATE-FAIL-CLOSED-IO** — Fail-closed on truncation corruption and I/O failures
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-RECOVERY-FAILCLOSED-001`
  Residual risk: System-level disk-full/quota/writeback matrices open
  Deterministic fault seams exist and run in ci.yml; system-level matrices open.

- [x] **GATE-RECOVERY-DETERMINISTIC** — Deterministic recovery after process termination
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-RECOVERY-DET-001`
  Residual risk: Randomized coverage samples bounded compaction checkpoint classes; native exhaustive filesystem and arbitrary timing matrices remain open
  Deterministic crash suites and a retained reproducible randomized campaign provide E2 signals; neither is physical power-loss proof.

- [x] **GATE-WRITE-ORDER** — Write ordering synchronization and manifest publication
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-PERSIST-ORDER-001`, `GS-PERSIST-AMP-001`
  Residual risk: The formal proof is an abstract bounded model; filesystem/power-loss certification matrices remain open and E3/E4 are not certified
  Platform-aware publication with fault and process-kill tests plus a required bounded write/sync/slot/Manifest/recovery model; write-amp and temporary-space budgets reject before compaction intent (GS-PERSIST-AMP-001).

### Verification

- [x] **GATE-DOCUMENTATION-INTEGRITY** — Tracked documentation has valid repository-local references
  State: `PROVATA_IN_CI` · Release target: `prototype`
  Requirements: `GS-CORE-DOCS-001`
  Residual risk: External URL availability and fragment validity remain scheduled Lychee checks; the deterministic gate covers repository-local targets on every assurance run
  The repository-local validator covers every tracked Markdown file, while Lychee retains the network-dependent user-documentation check.

- [x] **GATE-FAULT-INJECTION** — Fault injection for allocation filesystem clock socket thread failures
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-RECOVERY-FAILCLOSED-001`, `GS-PERSIST-FAULT-001`, `GS-PERSIST-AMP-001`
  Residual risk: Exhaustive socket/thread/clock/hardware power-cut and per-request block tracing remain open; E3 requires a retained pinned campaign
  Allocation and FS publication seams exist with EINTR/short-I/O/ENOSPC/EIO/EROFS FileIoHooks coverage across mutation, compaction staging+intent+promotion, and backup. The E3 rehearsal confirms a paused worker before reset and distinguishes bounded dm-flakey drop-write/write-error/all-I/O-error modes; platform-durability evidence paths remain placeholders and E3 honesty is enforced in CI.

- [x] **GATE-FUZZ-CI** — Fuzz targets run in CI with retained corpora
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-RECOVERY-FAILCLOSED-001`
  Residual risk: Bounded smoke only; multi-hour continuous fuzz open
  Bounded libFuzzer smoke in sanitizers workflow.

- [x] **GATE-PERFORMANCE** — Performance tests track latency throughput memory regressions
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-PERF-REGRESSION-001`, `GS-PERF-BUDGET-001`
  Residual risk: Absolute hardware budgets wait for glifistore-linux-perf pass-candidate
  Environment-compatible hosted median signals + budget catalog landed; hard regression and absolute claims remain hardware-gated.

- [x] **GATE-SOAK** — Long-running stress and soak coverage
  State: `PROVATA_IN_CI` · Release target: `beta`
  Requirements: `GS-OPS-CONFIG-001`, `GS-OPS-SOAK-001`, `GS-OPS-DEBT-001`
  Residual risk: Controlled multi-hour hardware soak with mandatory rotation evidence open; adversarial soak profiles (hot-key / connection-churn / queue-saturation / adversarial-reclaim) are software stubs only

  Smoke/long soaks exist and are budget-linked; Wave 4 adversarial profile stubs exercise shape + SIGTERM drain; hardware soak remains release residual.

- [x] **GATE-STATIC-ANALYSIS-HIGH-SIGNAL** — Production sources pass fail-closed high-signal static analysis
  State: `PROVATA_IN_CI` · Release target: `prototype`
  Requirements: `GS-CORE-BUILD-001`
  Residual risk: The all-target profile intentionally remains diagnostic for lower-confidence, test-macro-sensitive, and toolchain-sensitive findings; production fail-closed tidy runs on every static-analysis.yml push/PR for main
  The gate derives production translation units from compile_commands.json and rejects unchecked optional access, use-after-move, analyzer dead stores, and declaration/definition parameter-name drift without claiming that the wider diagnostic profile is warning-free.

- [x] **GATE-TEST-SUITES** — Distinct unit integration property concurrency crash recovery suites
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-RECOVERY-DET-001`, `GS-COMPAT-FIXTURE-001`
  Residual risk: Regular tagged artifact drops remain open
  Suites are distinct in tree; permanent tagged drops open.

### Operations and security

- [ ] **GATE-AUTH-TRANSPORT** — Authentication authorization transport security rate limits audit
  State: `PROVATA_LOCALMENTE` · Release target: `beta`
  Requirements: `GS-SEC-PROFILE-001`, `GS-SEC-BOUNDS-001`
  Residual risk: Multi-tenant Phase 8 remainder unsupported/deferred (ADR 0028, at-rest crypto); hostile-public CRL ops residual; restart-only TLS cert material; best-effort bounded audit may suppress events or lose pending output at process exit; fresh hosted ADR 0041 evidence pending

  Secure profile Phases 2–6 landed; Phase 8 remainder documented as unsupported/deferred (Wave 4 honesty). Reopened by ADR 0041 security fixes: previous CI evidence does not prove the new TLS/output/admission/audit lifecycle.

- [x] **GATE-CONFIG** — Configuration precedence validation safe defaults limits
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-OPS-CONFIG-001`
  Residual risk: Operator misuse of unsupported filesystems; multi-hour adversarial reclaim fairness soak open (Wave 4 adversarial-reclaim profile is a software stub only); intentional Prometheus/OpenMetrics non-support remains documented residual

  Validated durable defaults and daemon profiles; HAZ-026 reclaim cursor-advance proofs in CI.

- [x] **GATE-OPS-RUNBOOKS** — Graceful drain overload backup restore corruption runbooks
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-OPS-BACKUP-001`, `GS-OPS-CONFIG-001`, `GS-OPS-SOAK-001`, `GS-OPS-DEBT-001`
  Residual risk: Staging/production rehearsal still operator-owned
  Operator procedures exercised by ops-runbooks CI and linked via perf/ops budgets.

- [x] **GATE-TELEMETRY** — Structured logs metrics health readiness diagnostics
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-OPS-CONFIG-001`
  Residual risk: Histogram approximations are not SLOs; OpenMetrics/Prometheus exporter is intentional non-support (docs/operations/observability.md)

  Wire HEALTH/READY/STATS, JSON lifecycle logging, and operator observability catalog exist; no first-party Prometheus scrape endpoint.

- [ ] **GATE-THREAT-SUPPLY** — Threat model and security release process including supply chain
  State: `IMPLEMENTATA` · Release target: `beta`
  Requirements: `GS-SEC-PROFILE-001`, `GS-SUPPLY-ACTIONS-001`
  Residual risk: Project GPG / full SLSA L3 optional; Dependabot must update SHAs; no tagged security-matrix evidence has been retained yet
  Threat model, SBOM/checksum/Cosign/SLSA path, SHA-pinned Actions and Scorecard/dependency-review are implemented; the tag graph now closes the Linux sanitizer, CodeQL, static, supply-chain, SBOM and distributed-ELF matrix, but retained tagged evidence remains absent.

### Distribution and lifecycle

- [ ] **GATE-ARTIFACT-DELIVERY** — Candidate Verify Publish preserves exact artifact identity
  State: `IMPLEMENTATA` · Release target: `rc`
  Requirements: `GS-RELEASE-ARTIFACT-001`
  Residual risk: Native BSD package bytes, registered service accounts, a prior tagged Store, a complete attested prior ABI/wire release, and retained tagged evidence are still missing
  Exact candidate admission and a closed same-run evidence import remove the candidate-seal/evidence cycle; SDK, persistence, bidirectional C-ABI, bidirectional wire, the complete Linux security matrix, native FreeBSD and OpenBSD package lifecycles and independent reproducibility evidence have same-run producers; both BSD producers generate non-circular distinfo from the sealed source and verify account-registration markers; persistence rejects self/current fixtures; ABI/wire require an attested complete prior release and retained compiled consumers; rebuilt comparison bytes cannot enter promotion; publish cannot rebuild or clobber; and policy deliberately prevents release until the remaining proofs exist.

- [x] **GATE-CMAKE-INSTALL** — CMake installs versioned package metadata and GlifiStore::core
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-CORE-API-001`, `GS-CORE-BUILD-001`
  Residual risk: None beyond prototype claim ceiling; WAV-001 size debt closed
  Installed targets exist; Phase C subdirectory split and size-debt splits landed.

- [x] **GATE-INSTALL-CONSUMER** — CI builds external consumer from installed prefix
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-CORE-API-001`, `GS-COMPAT-CABI-001`
  Residual risk: Same-SHA install-consumer scaffold (symbols + layout probe) is in ci.yml; cross-release old-binary × new-library remains open
  Install-consumer job builds supported C++ targets and the pure-C ABI target, then runs check_abi_symbols and c_abi_layout_probe against the installed prefix; cross-release proof remains open (wave5-l7-residuals.md).

- [ ] **GATE-PACKAGE-ADMISSION** — Package upgrade baselines and package artifacts are admitted by exact sealed bytes
  State: `IMPLEMENTATA` · Release target: `rc`
  Requirements: `GS-RELEASE-UPGRADE-001`, `GS-RELEASE-ARTIFACT-001`
  Residual risk: Admission is wired fail-closed for tool execution and retains blockers; Linux nightly 34666166603 retained package-installed cross-SDK PASS for deb and rpm, but admitted stays false until package-upgrade is exercised against a sealed SemVer N-1 predecessor on required backends; Linux, FreeBSD/OpenBSD, and MacPorts/Homebrew implement install→seed→upgrade→verify when GLIFISTORE_N1_PACKAGE_DIR supplies those bytes (never rebuilt from HEAD), but no prior annotated release exists yet so the check stays NOT_APPLICABLE_INITIAL_BASELINE; no annotated release has retained a positive admission yet
  The baseline resolver orders annotated published releases by SemVer precedence, keeps only complete sealed ABI-compatible candidates, and separates NOT_APPLICABLE_INITIAL_BASELINE, BLOCKED and NOT_RUN; admission binds the artifact manifest to the sealed source digest and every evidence subject to the exact bytes, reporting blockers instead of promoting anything. package-ci.yml and release.yml invoke run_package_admission.py and retain the report. Cross-SDK package-installed PASS is retained for deb/rpm nightly; positive admission remains open until a sealed N-1 package-upgrade PASS exists on required backends. Linux, FreeBSD/OpenBSD, and MacPorts/Homebrew implement that walk when sealed predecessor packages (or macOS source archives) are supplied; a retained PASS still requires published N-1 bytes.

- [ ] **GATE-PACKAGE-LIFECYCLE** — Package backends declare and prove their lifecycle through honest evidence
  State: `IMPLEMENTATA` · Release target: `rc`
  Requirements: `GS-RELEASE-PACKAGE-001`, `GS-RELEASE-ARTIFACT-001`
  Residual risk: Retained nightly Package CI run 34662210614 (tip 58167e8) proved deb and rpm LIFECYCLE_VERIFIED with service-lifecycle PASS under systemd PID 1; overall result remains NOT_RUN without a sealed candidate, and required_for_release stays false. No retained MacPorts/Homebrew native lifecycle; both declare init integrations (unprivileged launchd startupitem.user/group; brew services) exercised when native runs, otherwise OPEN_GATE; every upstream acceptance stays OPEN_GATE; BSD external consumer is implemented but unretained on a tagged release; Linux, FreeBSD/OpenBSD, and MacPorts/Homebrew implement install→seed→upgrade→verify when GLIFISTORE_N1_PACKAGE_DIR supplies sealed N-1 packages (or macOS source archives), but no prior annotated release exists yet so package-upgrade stays NOT_APPLICABLE_INITIAL_BASELINE; deb, rpm, macports and homebrew remain required_for_release false
  One matrix owns backends, targets, profiles and the check vocabulary; scripts/package-ci.sh and its backend modules own every status; evidence is schema-bound to commit, version and artifact digest and may report NOT_RUN, BLOCKED or OPEN_GATE but never a result better than its checks; docs/distribution/package-status.md is generated from the matrix so no hand table can diverge. Retained nightly deb/rpm evidence reaches LIFECYCLE_VERIFIED; package-upgrade against sealed N-1 bytes, sealed-candidate admission, MacPorts/Homebrew native retention and every upstream acceptance remain open. required_for_release stays false.

- [ ] **GATE-RELEASE-MATRIX** — Release CI covers supported compilers OS arch optimized builds
  State: `IMPLEMENTATA` · Release target: `rc`
  Requirements: `GS-COMPAT-FIXTURE-001`, `GS-RELEASE-ARTIFACT-001`
  Residual risk: No immutable tag or native BSD package evidence exists; Windows is out of scope
  Platform build/test workflows exist; FreeBSD and OpenBSD package evidence producers are wired fail-closed in release.yml; tag publication is blocked until exact native packages and same-commit evidence enter the sealed manifest.

- [ ] **GATE-REPRO-SBOM** — Artifacts reproducible signed checksummed with provenance and SBOM
  State: `IMPLEMENTATA` · Release target: `rc`
  Requirements: `GS-COMPAT-FIXTURE-001`, `GS-RELEASE-ARTIFACT-001`
  Residual risk: First successful tagged independent comparison, full native BSD artifacts, and cross-image/compiler reproducibility evidence remain absent
  Deterministic source/install/ABI/wire archives, an isolated second-runner byte comparison, SPDX validation, transitive seals and OIDC attestation are implemented; rebuilt bytes cannot enter promotion and release policy remains intentionally red until retained tag and platform evidence exist.

- [x] **GATE-VERSION-LIFECYCLE** — Upgrade downgrade deprecation support EOL policies published
  State: `PROVATA_IN_CI` · Release target: `alpha`
  Requirements: `GS-COMPAT-FIXTURE-001`, `GS-COMPAT-NN1-001`
  Residual risk: Formal support windows for beta/RC/stable remain P3; tagged N−1 fixture drops residual
  0.x / persistence v1 policies + N↔N-1 matrix published; formal windows later.

## Change discipline

Any change to routing, hashing, persisted bytes, protocol framing, acknowledgement
semantics, or reclamation requires an ADR and new compatibility or recovery
evidence. Performance changes must preserve all safety and durability gates;
benchmark improvement is never evidence of correctness.

Assurance catalog: [`engineering/`](../engineering/) · Baseline:
[`docs/assurance/engineering-baseline.md`](assurance/engineering-baseline.md) ·
Agent rules: [`AGENTS.md`](../AGENTS.md).
