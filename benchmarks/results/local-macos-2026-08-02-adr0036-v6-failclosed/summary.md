# ADR 0036 V6 production fail-closed + V4 cold I/O baseline

Date: 2026-08-02
Preset: `macos-release` / `macos-asan` / `macos-tsan` (Apple Silicon)
Claim ceiling: lab evidence; production stays `shared_ptr` + ReadLease (no slot-pool land).

## Correctness fix

Async and sync durable Writer exception paths in `shard_pair_runtime.cpp` now sticky
fail-close once durable mutate has been entered (or a commit was observed), matching the
sync `publication_required` rule. Prevents unpublished committed durable state and
publish-after-client-error (inverted RAW).

## Commands

```text
./build/macos-release/glifistore_tests 'paired durable Writer fail-closes'
./build/macos-asan/glifistore_tests 'paired durable Writer fail-closes'
./build/macos-release/glifistore_tests 'paired Reader refreshes'
./build/macos-release/glifistore_tests 'durable cold read pin'
./build/macos-tsan/glifistore_tests 'paired Store concurrent GET observes'
```

## Results

| Check | Result |
| --- | --- |
| V6 production litmus (Release + ASan) | pass |
| V4 durable refresh / cold pin | 3/3 pass |
| V3 overwrite-storm under TSan | pass |
