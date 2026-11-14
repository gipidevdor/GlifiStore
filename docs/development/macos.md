# macOS and Xcode

**Descriptive** · macOS on Apple Silicon, Xcode and the generated CMake project

How to set up, build, test and profile GlifiStore on macOS with Xcode. macOS on Apple Silicon is the
primary development environment: CMake keeps build generation reproducible, while Xcode supplies
editing, debugging, sanitizers, Instruments and source navigation.

## One-time setup

Install full Xcode, open it once to accept its license, then run:

```bash
./scripts/bootstrap-macos.sh
```

The script verifies the selected Xcode developer directory and creates `.tools/venv` containing
CMake and Ninja, so no `sudo`, Homebrew or MacPorts is required. The generated project is
`build/xcode/GlifiStore.xcodeproj`.

The `.xcodeproj` is generated state and is not committed. CMake remains the source of truth, so
rerunning either the setup or the daily open command safely refreshes targets, schemes, headers,
scripts and documentation in the Xcode navigator. `scripts/generate-xcode.sh` also removes obsolete
shared schemes left by older generations while preserving personal `xcuserdata`, then recreates only
the repository-supported schemes.

If `xcode-select -p` points at Command Line Tools instead of full Xcode:

```bash
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

## Daily Xcode workflow

```bash
./scripts/open-xcode.sh
```

Select one of the generated schemes:

| Scheme | Use |
| --- | --- |
| `glifistore_demo` | Inspect detected topology in Debug |
| `glifistored` | Run the daemon on `127.0.0.1:7379` with two Workers |
| `glifistore_tests` | Build or run the standalone test executable |
| `glifistore_allocation_fault_tests` | Allocation fault-injection suite; not generated under TSan, whose runtime owns `operator new` |
| `check` | Build the test dependencies and execute the complete CTest suite |
| `glifistore_benchmarks`, `glifistore_server_benchmarks` | Focused Release measurements |
| `glifistore_paired_benchmark`, `glifistore_compaction_benchmark`, `glifistore_maintenance_benchmark` | Release microbenchmarks for the paired-shard prototype, durable compaction and the maintenance controller |
| `glifistore_inspect_segment` | Read-only durable Segment validation (`--json`, `--no-scan`) |
| `glifistore_verify_store` | Manifest and catalog Segment validation (exclusive lock) |
| `glifistore_backup_store` | Offline verified backup and restore copies |
| `glifistore_migrate_store` | Offline Worker-count change that copies live keys into a new Store (`--workers N`) |
| `glifistore_repair_store` | Offline repair into an explicit workspace (`store/` + `quarantine/`) |
| `glifistore_interop_client` | Wire-v2 C++ client that the SDK interop and operations scripts drive |
| `glifistore_rebuild_index` | Permanently refuses Segment-only durable v1 input; reopen or restart the Store for ordinary Index rebuild, or use `glifistore_repair_store` for offline catalog repair |

Crash labels: `glifistore_crash_persistence` covers Store filesystem boundaries, and
`glifistore_crash_daemon` a real `glifistored` SIGKILL after wire acknowledgements.

GlifiStore uses a standalone CTest executable rather than XCTest bundles. Use the Build action of
the `check` scheme for the full suite; the Run action of the `glifistore_tests` scheme launches the
test process directly. Xcode's XCTest-specific Test action is not used.

Use Product > Scheme > Edit Scheme to add temporary diagnostics locally. Do not commit `xcuserdata`.
For repeatable sanitizer runs, prefer the repository presets:

```bash
./scripts/dev.sh asan
./scripts/dev.sh tsan
```

To verify the complete generated project from the command line:

```bash
./scripts/verify-xcode.sh
# or: make xcode-verify
```

This regenerates the project, builds `ALL_BUILD` in Debug and Release, runs the `check` scheme and
smoke-tests `glifistore_demo`.

## Instruments

Use `RelWithDebInfo` before profiling when symbols are required. In Xcode, choose Product > Profile
and use:

- Time Profiler for hot paths;
- Allocations for unexpected heap traffic;
- Counters for cache and branch behavior, where supported;
- System Trace for scheduling and I/O effects.

Never publish benchmark results from a Debug, sanitizer, Rosetta or thermally throttled run. Record
the Mac model, CPU, OS, Xcode/Clang, payload distribution, dataset size and build commit.

## Apple Silicon and Intel

CMake defaults to the host architecture. To cross-generate locally, create an untracked
`CMakeUserPresets.json` that inherits from `xcode` and sets `CMAKE_OSX_ARCHITECTURES` to `arm64` or
`x86_64`. Universal binaries are not the default because they slow the inner development loop and
hide architecture-specific optimization decisions.

## Make targets

The root `Makefile` mirrors every `./scripts/dev.sh` command; plain `make` prints the list:

```bash
make bootstrap
make doctor
make xcode
make xcode-verify
make test
make asan
make verify
make benchmark
```

## Troubleshooting

- Start with `./scripts/dev.sh doctor`: it shows the resolved tools and warns about Finder and
  iCloud conflict copies. Do not keep the checkout in iCloud-synced `~/Documents` or `~/Desktop`;
  iCloud duplicates files as `name 2.ext`, including inside `.git/`.
- Regenerate after changing CMake: `./scripts/open-xcode.sh`.
- Remove generated state with `./scripts/dev.sh clean`, then bootstrap again.
- If Python cannot create a virtual environment, install a current Python 3 or provide `CMAKE` and
  `CTEST` environment variables pointing at existing tools.
- A TSan failure is not waived merely because the same test passes normally.
- Some Apple Xcode distributions do not ship a linkable macOS libFuzzer runtime; the normal, ASan
  and TSan presets still use Apple Clang. To build fuzz targets locally, install a complete LLVM
  toolchain and configure `macos-fuzz` with `CC`/`CXX` pointing at that Clang. CI builds and runs
  the fuzzers on Linux (`sanitizers.yml` `fuzz-run`) with a complete runtime and seed corpora under
  `fuzz/corpus/`. After a local fuzz build, run
  `GLIFISTORE_FUZZ_BUILD_DIR=build/macos-fuzz ./scripts/dev.sh fuzz-run`.
