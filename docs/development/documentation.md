# Documentation

**Normative** · writing and review rules; the site build is descriptive · reviewed 2026-11-14

How GlifiStore documentation is written, reviewed, built and published. Documentation is part of
the product: it carries the same contracts as the code and the same claim ceiling, architectural
prototype.

## Write

Write the least that lets the reader act correctly. Remove whatever the reader would not miss.

### Shape

- **One page, one purpose.** A page settles one question for one reader. Two purposes make two
  pages; two pages with one purpose become one. Every fact has a single home; elsewhere, link to it.
- **Title.** A noun phrase in sentence case: `# Durable compaction`, not
  `# Guide to crash-safe durable compaction`. Version numbers appear only in versioned contracts:
  `# Persistence v1`.
- **Status line.** Directly under the title, one line:

  ```markdown
  **Normative** · persistence format family v1 · reviewed 2026-07-19
  ```

  The status is **Normative** (required behavior), **Descriptive** (what the implementation does),
  **Roadmap** (planned work) or **Historical** (a dated record). The scope names the API, format,
  wire or implementation version the page applies to. The date is the last technical review, not
  the last edit; leave it out when no review is recorded. A Historical page carries the date of the
  record itself, without the word "reviewed". Indexes, ADRs and generated pages keep their own
  headers.
- **Opening.** One or two sentences that say what the page settles, so the reader can decide
  whether to read on.
- **Order.** Contract before mechanism, mechanism before history. No introduction that announces
  the sections, no conclusion that repeats them.

### Voice

- Present tense, active voice, declarative sentences. One idea per paragraph.
- Name things exactly as the [glossary](../glossary.md) defines them: Store, Worker, shard pair,
  Reader, Writer, Index, Segment, Record, Manifest.
- State limits as plainly as capabilities. Never imply more than the claim ceiling.
- No intensifiers or promotion: *very*, *simply*, *just*, *fully*, *robust*, *seamless*,
  *powerful*.
- No project bookkeeping in guides: waves, lanes, phases, priority labels and dated narratives
  belong in trackers, the changelog and Git history. Stable identifiers stay: requirement, hazard,
  gate, waiver and ADR IDs.
- Compare GlifiStore only with its own baselines, under the
  [benchmark standard](../spec/benchmark-standard.md). Do not name other products.
- English throughout.

### Form

- Prose explains, lists enumerate, tables compare. No single-column table, no one-item list.
- Bold marks the one phrase a reader must not miss, rarely more than once per section. No emoji,
  exclamation marks or decorative rules.
- GitHub callouts (`> [!WARNING]`) belong to the repository README only.
- Code blocks name their language and run as written.
- Link text names the destination, as in `[persistence v1](../spec/persistence-v1.md)`, never
  "here".
- Quantities carry units: 64 MiB, 4 KiB, 32 requests, 763 µs.
- Prose wraps at 100 columns.
- Pages under `docs/` are also built by Doxygen: keep `@` and `\` inside code spans, and keep any
  heading that another page links to by fragment.

## Review

A change must update the documentation when it changes:

- a public API precondition, postcondition, error, lifetime or thread-safety guarantee;
- a disk or wire byte;
- a commit, acknowledgement, visibility or recovery point;
- Worker routing or ownership;
- a lock order, atomic synchronization edge or shutdown transition;
- a benchmark workload or the meaning of one reported operation;
- a fixed architectural decision.

A disk, wire or API change also updates, in the same change, the affected specification, its ADR
relationships, the fixtures and the compatibility matrix.

A page is done when its reader can make the decision or perform the operation it names without
reading source or asking its author, its examples and tests agree with it, and no higher authority
contradicts it. When code and a specification disagree, record the discrepancy and resolve it
through an ADR; never edit either side silently.

`python3 engineering/tools/validate_documentation.py` checks every repository-local link in tracked
Markdown; run it after moving or renaming a page. A scheduled Lychee workflow checks network links.

Every release should publish an immutable snapshot of these pages with its source tag. Benchmark
records and roadmaps may expire; specifications for readable disk and wire versions must not.

Review cadence:

- monthly during active development, check roadmaps and readiness statements for stale completion
  claims;
- before every release, verify local links and record the exact commit in benchmark and evidence
  records;
- annually after 1.0, review terminology, platform evidence, deprecated API, migration paths and
  whether each accepted ADR still has an explicit successor relationship.

Open documentation work: a performance-history policy, an allocation and residency reference, a
fuzzing corpus guide and guided contributor exercises.

## Build the site

The site combines a Doxygen reference generated from `include/glifistore/` with the guides under
`docs/`, styled by Doxygen Awesome: responsive sidebar, in-browser search, light and dark themes,
copy buttons on code. Generating or publishing it closes no assurance gate, and indexing a header
does not make its symbols part of the supported or installed API; the
[reference contracts](../reference/cpp-api.md) define that boundary.

It needs CMake 3.25+, Python 3.10+ and Doxygen 1.18.0+. Graphviz, a C++ compiler, TLS libraries and
an engine build are not required. Theme assets are vendored, so generation works offline.

macOS:

```bash
brew install cmake doxygen python
```

Ubuntu 24.04 (x86-64) ships Doxygen 1.9.8, whose links fail the site checks. Install the official
1.18.0 binary and verify its [published SHA-256](https://www.doxygen.nl/download.html), as CI does:

```bash
sudo apt-get update
sudo apt-get install cmake python3
mkdir -p .tools
curl --fail --location \
  https://github.com/doxygen/doxygen/releases/download/Release_1_18_0/doxygen-1.18.0.linux.bin.tar.gz \
  --output .tools/doxygen-1.18.0.linux.bin.tar.gz
echo '14fa81bdc34171edb5f1f02b1d60e74802f0439b77fa44e592565d517d72df90  .tools/doxygen-1.18.0.linux.bin.tar.gz' | sha256sum --check -
tar -xzf .tools/doxygen-1.18.0.linux.bin.tar.gz -C .tools
export PATH="$PWD/.tools/doxygen-1.18.0/bin:$PATH"
```

Build and serve from the repository root:

```bash
cmake -S docs/doxygen -B build/docs
cmake --build build/docs --target glifistore-docs
python3 -m http.server 8000 --directory build/docs/html
```

Open `http://localhost:8000`. An HTTP server makes search and clipboard behave as on the hosted
site; the entry point on disk is `build/docs/html/index.html`.

The main build exposes the same optional target:

```bash
cmake --preset unix-debug -DGLIFISTORE_BUILD_DOCS=ON
cmake --build --preset unix-debug --target glifistore-docs
```

The site is then under `build/unix-debug/docs/doxygen/html`, or `docs/doxygen/html` beneath the
build directory the preset selects. Normal engine builds need neither Doxygen nor Python for
documentation.

## Publish

1. Push the documentation changes.
2. In **Settings → Pages → Build and deployment**, select **GitHub Actions** as the source.
3. Push to `main`, or run **Actions → C++ documentation → Run workflow** on `main`.
4. Read the deployed URL from the workflow's `github-pages` environment.

For the `gipidevdor/GlifiStore` origin the default address is
`https://gipidevdor.github.io/GlifiStore/`, unless the Pages settings name a custom domain. A local
commit starts the workflow only once it is pushed. Pull requests generate and validate the site;
only `main` deploys, and every push to `main` (including a merged pull request) republishes from
that commit. Implementation-only changes also rebuild the site; descriptions reflect the
declarations and annotations in that commit. Only the deployment job receives `pages: write` and
`id-token: write`, through GitHub's official Pages actions pinned to full commit SHAs; no personal
access token or `gh-pages` branch is involved.

The workflow points source and evidence links at the repository and commit being built. For a local
build against another fork or revision:

```bash
cmake -S docs/doxygen -B build/docs \
  -DGLIFISTORE_DOCS_SOURCE_URL=https://github.com/OWNER/REPOSITORY \
  -DGLIFISTORE_DOCS_REVISION=main
```

## Maintain the site

- Put Doxygen comments (`///`, `/** ... */`, `@brief`, `@param`, `@return`, `@see`) next to
  declarations. Document ownership, thread safety, errors and observable guarantees from the
  accepted contracts; changing those guarantees follows the ADR discipline.
- The landing page is [`docs/doxygen/index.md`](../doxygen/index.md), the generator settings are in
  [`Doxyfile.in`](../doxygen/Doxyfile.in) and the colors in [`custom.css`](../doxygen/custom.css).
- Edit guides where they live. The builder stages disposable copies, turns guide links into Doxygen
  navigation and links other repository files to GitHub. It leaves fenced code untouched and never
  rewrites the source guides or the generated assurance views.
- The build fails on Doxygen warnings, missing local pages or assets and broken content fragment
  links. Doxygen's own alphabet-index fragments are excluded from fragment checks; their pages are
  still checked. Prose completeness and remote URLs are outside these checks.
- Generated HTML stays under the ignored `build/` directory and is never committed.

The vendored theme is Doxygen Awesome **v2.5.0** at commit
`46483f1e5a70ffb9ecd3b82d0a1cd1b24edf13da`, under its MIT [license](../doxygen/theme/LICENSE),
which is copied into every generated site. Update its CSS and JavaScript together, keep the
attribution, and check the site with both the minimum supported and the local Doxygen version.
