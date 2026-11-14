# Local native code style campaign — 2026-11-07

Evidence class: **local**, on macOS ARM64. GlifiStore remains an architectural prototype.
No hosted CI, release, durability or numerical perfection claim is made by this record.

## Subject

This campaign applies [the native code style](../../docs/development/code-style.md), pins
clang-format 21.1.8 through one [version file](../../.clang-format-version), and gives bootstrap,
local commands and CI one formatter entry point. It extends the production clang-tidy gate from
four to eight selected checks. AGENTS.md links the rules for subsequent work.

The first formatting pass updated 135 files. Follow-up passes included newly appearing sources;
the final retained check selected **414 files with zero formatting differences**. Coverage includes
C++/C headers and sources, tools, ABI consumers, packaging consumers and diagnostic probes.
The configuration preserves qualifiers and statement tokens; the control-flow brace repairs were
applied using clang-tidy's AST fixes and reviewed as cosmetic changes.

Concurrent blob-storage, compaction, documentation and workflow work is outside this cosmetic
campaign's implementation subject. Its source changes are preserved. The formatter checks the file
contents again before writing and fails if it observes an intervening save or a changed inventory.

## Local checks

| Check | Result | Retained output |
| --- | --- | --- |
| Pinned formatting | 414 native files, no drift after formatting | [Check](code-style-2026-11-07/format-check.log), [initial pass](code-style-2026-11-07/format-write.log) |
| Extended high-signal clang-tidy | 112 production sources, 8 checks, no failures | [Gate output](code-style-2026-11-07/clang-tidy.log) |
| Python tooling suite | 446 tests passed | [Full suite](code-style-2026-11-07/python-tests.log) |
| Focused tests after the final inventory-race regression | 23 tests passed | [Focused suite](code-style-2026-11-07/focused-tests.log) |
| Exact compile-command syntax checks | 8 native tools plus the C11 ABI consumer passed | [Compiler output](code-style-2026-11-07/native-syntax.log) |
| Workflow, shell, structure, documentation and assurance validation | Passed | [Validator output](code-style-2026-11-07/validators.log) |
| Full CMake rebuild | Not completed: concurrent blob recovery/storage calls private `DataDirectory::sync_directory()` | [Build output](code-style-2026-11-07/build.log) |

The [machine-readable summary](code-style-2026-11-07/summary.json) records policy hashes and log
hashes. Test claims here are limited to those commands; the existing C++ executable was not reused
to imply that a failed rebuild had validated the new workspace state.

## Reproduction

```sh
./scripts/dev.sh format
./scripts/dev.sh format-check
python3 -m unittest discover -s scripts/tests
python3 -m unittest scripts.tests.test_code_format scripts.tests.test_dev_verify \
  scripts.tests.test_clang_tidy_gate scripts.tests.test_ci_workflow
python3 engineering/tools/run_clang_tidy_gate.py --build-dir build/macos-debug \
  --clang-tidy "$PWD/.tools/venv/bin/clang-tidy" --jobs 4
actionlint .github/workflows/static-analysis.yml
python3 engineering/tools/validate_assurance.py --write-generated
python3 engineering/tools/validate_structure_debt.py
python3 engineering/tools/validate_documentation.py
```

## Residual scope

The tidy row covers the 112 production sources present in the compile database at that check;
additional concurrent blob sources appeared later and are not covered by that green run.
The four additional style checks are locally verified; the modified workflow has not run in hosted
CI. Runtime suites, sanitizers and a complete rebuild need rerunning after the concurrent feature
files and CMake lists converge. Readability also requires review of names, comments and invariants;
a green formatter or a broad collection of lint switches does not certify all those properties.
