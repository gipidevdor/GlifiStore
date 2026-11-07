Status: descriptive
Applies to: generated C++ reference and GitHub Pages site
Owner: maintainers
Last reviewed: 2026-11-07

# Build and publish the C++ documentation

The documentation site combines a Doxygen reference generated from `include/glifistore/` with
the existing Markdown guides under `docs/`. It uses Doxygen Awesome for a responsive sidebar,
browser search, a light/dark switch, and copy buttons on code examples. API documentation stays
in English, consistent with the source and reference contracts.

GlifiStore remains an **architectural prototype**. Generating or publishing documentation does
not close assurance gates. Specifications, accepted ADRs, and
[reference contracts](../reference/cpp-api.md) retain their documented authority. Indexing a
header does not make its implementation symbols part of the supported or installed public API.

## Generate locally

Requirements: CMake 3.25+, Python 3.10+, and Doxygen 1.18.0+. Graphviz, a C++ compiler, TLS
libraries, and a build of the engine are not required for the standalone documentation target.
All theme assets are vendored; generation needs no network connection.

macOS:

```bash
brew install cmake doxygen python
```

Ubuntu 24.04 (x86-64), from the repository root:

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

The Ubuntu distribution package provides Doxygen 1.9.8, whose generated links do not satisfy
the site's checks. CI uses the official Doxygen 1.18.0 Linux binary and verifies its
[published SHA-256](https://www.doxygen.nl/download.html) before running it.

From the repository root:

```bash
cmake -S docs/doxygen -B build/docs
cmake --build build/docs --target glifistore-docs
python3 -m http.server 8000 --directory build/docs/html
```

Open `http://localhost:8000`. Use an HTTP server so browser search and clipboard behavior match
the hosted site. The entry point on disk is `build/docs/html/index.html`.

The main build can expose the same optional target:

```bash
cmake --preset unix-debug -DGLIFISTORE_BUILD_DOCS=ON
cmake --build --preset unix-debug --target glifistore-docs
```

With that configuration, the site is under `build/unix-debug/docs/doxygen/html` (or
`docs/doxygen/html` beneath the build directory selected by the preset).
Normal engine builds do not require Doxygen or Python for documentation.

## Publish with GitHub Pages

1. Commit and push the documentation changes to the repository.
2. In **Settings → Pages → Build and deployment**, select **GitHub Actions** as the source.
3. Push a commit to `main`, or run **Actions → C++ documentation → Run workflow** on `main`.
4. Read the deployed URL from the workflow's `github-pages` environment.

For the current `gipidevdor/GlifiStore` origin, the default address is
`https://gipidevdor.github.io/GlifiStore/`, unless repository Pages settings configure a custom
domain. The workflow generates and validates the site on pull requests; only `main` can deploy.
Every push to `main`, including a merged pull request, regenerates and publishes the site from
that commit. A local commit starts the workflow only after it is pushed to GitHub. The reference
is extracted from headers and Doxygen comments; implementation-only changes also trigger a fresh
build, while descriptions reflect the declarations and annotations present in that commit.
The deployment job alone receives `pages: write` and `id-token: write`. It uses GitHub's official
Pages artifact/deployment actions, pinned to full commit SHAs; no personal access token or
generated `gh-pages` branch is needed.

The workflow sets source/evidence links to the repository and commit being built. For a local
build against another fork or revision:

```bash
cmake -S docs/doxygen -B build/docs \
  -DGLIFISTORE_DOCS_SOURCE_URL=https://github.com/OWNER/REPOSITORY \
  -DGLIFISTORE_DOCS_REVISION=main
```

## Maintain the site

- Add Doxygen comments (`///`, `/** ... */`, `@brief`, `@param`, `@return`, `@see`) next to
  declarations. Document ownership, thread safety, errors, and observable guarantees from the
  accepted contracts; changes to those guarantees follow the normal ADR discipline.
- Edit the landing page in [`docs/doxygen/index.md`](../doxygen/index.md), the generator settings
  in [`Doxyfile.in`](../doxygen/Doxyfile.in), and project colors in
  [`custom.css`](../doxygen/custom.css).
- Edit guides in their existing locations. The builder stages disposable copies, converts
  guide links into Doxygen navigation, and links other repository files to GitHub. Fenced code
  examples remain unchanged. It never rewrites the source guides or generated assurance views.
- The build fails on Doxygen warnings, missing local pages/assets, and broken content fragment
  links. Doxygen's generated alphabet-index jump fragments are excluded from fragment checks;
  their destination pages are still checked. This does not certify inline prose completeness
  or check remote URLs; the existing documentation checks retain their roles.
- Generated HTML stays under the ignored `build/` directory. Do not commit it.

The vendored theme is Doxygen Awesome **v2.5.0**, commit
`46483f1e5a70ffb9ecd3b82d0a1cd1b24edf13da`. Its original MIT
[license](../doxygen/theme/LICENSE) is retained and copied into every generated site.
When updating it, replace the CSS/JavaScript files together, retain attribution, and verify the
site with the minimum supported Doxygen version and the local version.
