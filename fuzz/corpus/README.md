# Fuzz seed and regression corpora

Committed seeds under `fuzz/corpus/<target>/` bootstrap continuous libFuzzer runs. CI builds and
runs each target in the `fuzz-run` job of `.github/workflows/sanitizers.yml`, through
`scripts/run-fuzzers.sh`.

| Target | Binary | Corpus |
| --- | --- | --- |
| `record_decoder` | `fuzz_record_decoder` | `fuzz/corpus/record_decoder` |
| `segment_scanner` | `fuzz_segment_scanner` | `fuzz/corpus/segment_scanner` |
| `index_rebuild` | `fuzz_index_rebuild` | `fuzz/corpus/index_rebuild` |
| `protocol_decoder` | `fuzz_protocol_decoder` | `fuzz/corpus/protocol_decoder` |
| `store_state_machine` | `fuzz_store_state_machine` | `fuzz/corpus/store_state_machine` |

A local smoke run needs Linux Clang with the libFuzzer runtime:

```bash
cmake --preset unix-fuzz
cmake --build --preset unix-fuzz
GLIFISTORE_FUZZ_SECONDS=30 ./scripts/run-fuzzers.sh
```

On macOS, use a full LLVM toolchain and the `macos-fuzz` preset, then point
`GLIFISTORE_FUZZ_BUILD_DIR` at `build/macos-fuzz`.

When a minimized input catches a regression, promote it into these directories. Do not commit
libFuzzer crash-, timeout- or oom- artifacts; they stay gitignored.
