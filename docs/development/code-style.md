Status: normative for repository-owned native code
Applies to: ISO C++23 implementation and C11 ABI consumers
Owner: maintainers
Last reviewed: 2026-11-07

# Native code style

Use one reproducible style throughout production code, tests, tools, benchmarks, fuzz targets,
packaging consumers and diagnostic probes. Consistency makes invariants and exceptional paths
easier to review. The [agent guidance](../../AGENTS.md) applies these rules to future changes.

## Mechanical layout

The authority is [`.clang-format`](../../.clang-format), using the exact version in
[`.clang-format-version`](../../.clang-format-version). The formatter uses its `Latest` parser
mode because version 21 has no `c++23` spelling; the compiler still requires ISO C++23.

- Four spaces, no tabs, LF line endings and a final newline.
- A 110-column code layout, attached opening braces, and at most one empty line.
- Separate definitions with a blank line; keep access labels and switch labels consistent.
- Attach pointer/reference markers to the type; keep include blocks sorted and grouped.
- Preserve comment prose, qualifiers, braces, parentheses and semicolons during formatting.
  A formatter is not an authority for semantic transformations.

Use the same commands locally and in CI:

```sh
./scripts/dev.sh format
./scripts/dev.sh format-check
```

Portable direct entry points are `python3 engineering/tools/format_code.py --write` and `--check`.
The checker includes tracked files and new non-ignored files, handles paths containing spaces,
rejects the wrong formatter version, and excludes ignored build/tool output. `CLANG_FORMAT` or
`--clang-format /path/to/clang-format` selects a compatible executable. Bootstrap and CI install
the version from the same pin file. Avoid manual formatting that the pinned tool will undo.

## Names and interfaces

- Use `PascalCase` for types, `snake_case` for functions/variables/files, `kName` for constants,
  and a trailing underscore for private class data. Public aggregate members keep plain names.
- Make units visible: `timeout_ms`, `expire_at_ns`, `capacity_bytes`, `worker_count`. Boolean
  names express a predicate such as `healthy`, `accepting` or `has_pending_output`.
- Prefer domain names over vague abbreviations. Short conventional loop indices and narrowly
  scoped iterators are acceptable when their meaning is immediate.
- Declare dependencies directly instead of relying on transitive includes. Keep headers
  self-contained, use `#pragma once`, and use qualified names across subsystem boundaries.
- Keep parameter names consistent between declarations and definitions. Public APIs and wire/disk
  names are contracts: renaming them requires the repository's compatibility discipline.

## Control flow, types and ownership

- Brace every conditional and loop body. Prefer a clear guard clause over unnecessary nesting.
  Keep each function focused on one operation or invariant; split at meaningful boundaries.
- Use `const` for values that remain unchanged. Keep move sources mutable. Use `auto` where the
  initializer makes the type clear; spell types at interfaces and where widths or units matter.
- Use `nullptr`, scoped enums and checked arithmetic. A cast must express a justified conversion;
  it must not merely silence a diagnostic. Keep explicit little-endian codecs for persisted data.
- Use RAII and explicit owners, views and lifetime tokens. Preserve alignment, padding, atomics,
  memory orders and bounded-lane invariants when improving layout.
- Mark fallible results `[[nodiscard]]`; handle or deliberately document each ignored result.
  Use `noexcept` only when the entire path, including error construction, supports that guarantee.
- Explain *why* in comments: ownership, linearization, failure polarity, limits and non-obvious
  algorithms. Keep useful contract and evidence links; remove obsolete explanations when code
  changes. Avoid comments that only repeat the following statement.

These are review rules with context, not a request to rewrite idiomatic code into a single
syntactic pattern. Readability does not justify changing acknowledgement points, borrow lifetimes,
recovery behavior or external outcomes.

## Doxygen contracts

Keep API documentation in English next to declarations. Use `@file` for a header's role,
`@brief` for types and operations, and `@param`, `@return`, `@pre`, `@note`, or `@see` where they
clarify a contract. Use `///<` for concise aggregate-field descriptions. Ordinary implementation
comments explain algorithms and invariants; avoid narrating obvious statements or accessors.

Document who owns buffers and handles, when borrowed views become invalid, which thread may
access mutable state, and whether a result carries a lifetime pin. State units, zero/sentinel
meanings, admission limits, and relevant errors. Separate durable commit knowledge from read
publication and acknowledgement; an error must not be described as proof of rollback. Describe
partial progress and cleanup obligations where failure can leave private output behind.

Check comments against implementation and accepted specifications. A documented symbol is not
automatically a supported public API or evidence of a closed assurance gate. Keep literal paths
and placeholders in backticks so Doxygen does not parse them as commands or HTML. Generate the
site with the [documentation workflow](documentation.md) and resolve warnings and broken links
before handoff.

## Automated checks and review

The [production clang-tidy gate](../../engineering/tools/run_clang_tidy_gate.py) rejects unchecked
optional access, use after move, dead stores, declaration/definition drift, unbraced control flow,
duplicate includes, obsolete null constants and missing `override`. The broader
[clang-tidy profile](../../.clang-tidy) remains diagnostic where toolchain or macro behavior needs
triage. Enabling all available checks is not a substitute for reviewing their signal.

Use the [test strategy](test-strategy.md) for behavioral validation. A mechanical cleanup needs a
clean formatting check, a build with the supported warnings, appropriate tests and review of any
token changes such as include ordering. Run the fail-closed tidy gate after C++ edits. Keep cosmetic
changes distinct from fixes, retain local evidence honestly, and do not hide warnings through blanket
`NOLINT` or `clang-format off` blocks.
