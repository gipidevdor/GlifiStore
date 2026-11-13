# macOS and Xcode development

macOS on Apple Silicon is GlifiStore's primary development environment. The repository keeps
build generation reproducible through CMake while Xcode supplies editing, debugging, sanitizers,
Instruments, and source navigation.

## One-time setup

Install full Xcode, open it once to accept its license, then run:

```bash
./scripts/bootstrap-macos.sh
```

The script verifies the selected Xcode developer directory and creates `.tools/venv` containing
CMake and Ninja. This avoids requiring `sudo`, Homebrew, or MacPorts. The generated project is
`build/xcode/GlifiStore.xcodeproj`.

The `.xcodeproj` is generated state and is intentionally not committed. CMake remains the source
of truth, so rerunning either setup or the daily open command safely refreshes targets, schemes,
headers, scripts, and documentation in the Xcode navigator.

`scripts/generate-xcode.sh` also removes obsolete shared schemes left by older generations while
preserving personal `xcuserdata`, then recreates only the repository-supported schemes.

If `xcode-select -p` points at Command Line Tools instead of full Xcode:

```bash
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

## Daily Xcode workflow

```bash
./scripts/open-xcode.sh
```

Select one of these generated schemes:

- `glifistore_demo` to inspect detected topology in Debug.
- `glifistored` to run the daemon on `127.0.0.1:7379` with two Workers.
- `glifistore_tests` to build or run the standalone test executable.
- `check` to build the test dependencies and execute the complete CTest suite.
- `glifistore_benchmarks` and `glifistore_server_benchmarks` for focused Release measurements.
- `glifistore_inspect_segment` for read-only durable Segment validation (`--json`, `--no-scan`).
- `glifistore_verify_store` for Manifest + catalog Segment validation (exclusive lock).
- `glifistore_backup_store` for offline verified backup/restore copies.
- `glifistore_repair_store` for offline repair into an explicit workspace (`store/` + `quarantine/`).
- `glifistore_rebuild_index` permanently refuses Segment-only durable v1 input; reopen/restart
  the Store for ordinary Index rebuild, or use `glifistore_repair_store` for offline catalog repair.
- Crash labels: `glifistore_crash_persistence` (Store filesystem boundaries) and
  `glifistore_crash_daemon` (real `glifistored` SIGKILL after wire acknowledgements).

Because GlifiStore uses a standalone CTest executable rather than XCTest bundles, use the
`check` scheme's **Build** action for the full suite. The `glifistore_tests` scheme's **Run** action
launches the test process directly; Xcode's XCTest-specific **Test** action is intentionally not
used.

Use Product > Scheme > Edit Scheme to add temporary diagnostics locally. Do not commit
`xcuserdata`. For repeatable sanitizer runs, prefer the repository presets:

```bash
./scripts/dev.sh asan
./scripts/dev.sh tsan
```

To verify the complete generated project from the command line:

```bash
./scripts/verify-xcode.sh
# or: make xcode-verify
```

This regenerates the project, builds `ALL_BUILD` in Debug and Release, runs the `check` scheme, and
smoke-tests `glifistore_demo`.

## Instruments

Use `RelWithDebInfo` before profiling when symbols are required. In Xcode choose Product > Profile
and use:

- Time Profiler for hot paths.
- Allocations for unexpected heap traffic.
- Counters for cache and branch behavior where supported.
- System Trace for scheduling and I/O effects.

Never publish benchmark results from a Debug, sanitizer, Rosetta, or thermally throttled run.
Record Mac model, CPU, OS, Xcode/Clang, payload distribution, dataset size, and build commit.

## Apple Silicon and Intel

CMake defaults to the host architecture. To cross-generate locally, create an untracked
`CMakeUserPresets.json` inheriting from `xcode` and set `CMAKE_OSX_ARCHITECTURES` to `arm64` or
`x86_64`. Universal binaries are not the default because they slow the inner development loop and
hide architecture-specific optimization decisions.

## CLI shortcuts

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

- Start with `./scripts/dev.sh doctor`: it shows the resolved tools and warns about Finder/iCloud
  conflict copies. Do not keep the checkout in iCloud-synced `~/Documents` or `~/Desktop`; iCloud
  duplicates files as `name 2.ext`, including inside `.git/`.
- Regenerate after changing CMake: `./scripts/open-xcode.sh`.
- Remove generated state: `./scripts/dev.sh clean` and bootstrap again.
- If Python cannot create a virtual environment, install a current Python 3 or provide `CMAKE` and
  `CTEST` environment variables pointing at existing tools.
- A TSan failure is not waived merely because the same test passes normally.
- Some Apple Xcode distributions do not ship a linkable macOS libFuzzer runtime. The normal,
  ASan, and TSan presets still use Apple Clang. To build fuzz targets locally, install a complete
  LLVM toolchain and configure `macos-fuzz` with `CC`/`CXX` pointing at that Clang. CI builds and
  runs the fuzzers on Linux (`sanitizers.yml` `fuzz-run`) with a complete runtime and seed corpora
  under `fuzz/corpus/`. After a local fuzz build:
  `GLIFISTORE_FUZZ_BUILD_DIR=build/macos-fuzz ./scripts/dev.sh fuzz-run`.
