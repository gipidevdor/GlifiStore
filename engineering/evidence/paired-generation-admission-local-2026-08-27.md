# Paired generation admission — local macOS evidence (2026-08-27)

Status: local development evidence only. This record does not close a CI gate and does not certify
production readiness.

## Contract exercised

- one current read generation plus at most 64 retired generations per shard;
- the 65th publication attempt is rejected before Store mutation while a Reader pins the old
  frontier;
- the rejected key remains absent and the outcome is `resource_exhausted` / known-not-committed;
- a durable filesystem hook observes no `write_record` call for the rejected mutation;
- admission resumes after Reader quiescence;
- both embedded combiner and dedicated Writer synchronous paths use the same decision;
- the async lane admits the bounded task, returns the pre-Store rejection through its completion,
  preserves Writer epoch, releases the payload slot and resumes on the next Writer turn;
- a two-item durable-group batch rejects both elements without any filesystem write;
- refresh and merge paths compile against the same admission primitive;
- all 14 generation installation sites use one bounded fail-fast helper, so a future admission
  bypass cannot silently grow the retire graph;
- `generation_admission_backpressure_total` reports rejected mutations.

The local performance diagnostic for the internal guard is retained separately under
`benchmarks/results/generation-retire-install-guard-2026-08-27/`. It is below the 5% diagnostic
throughput rejection threshold but does not satisfy ADR 0036 V11/V12.

## Commands and results

```text
build/macos-release/glifistore_tests paired
51 tests, 0 failures

build/macos-asan/glifistore_tests paired
62 tests, 0 failures

build/macos-tsan/glifistore_tests paired
62 tests, 0 failures

.tools/venv/bin/python engineering/tools/validate_assurance.py
Assurance validation OK (30 requirements, 30 hazards, 26 gates, 2 waivers; 0 warnings)

git diff --check
clean
```

The ASan preset also enables UBSan in the repository configuration. This is a bounded local run,
not the multi-platform or long-duration evidence required for a release claim.
