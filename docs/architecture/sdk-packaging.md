# SDK packaging

**Normative** · official SDK packaging: Python, Perl, Go, Ruby, Erlang and the C++ client · reviewed 2026-07-24

Every official client must be buildable, installable, version-locked and verifiable without asking
a maintainer. Publishing to registries (PyPI, PAUSE, RubyGems, Hex, proxy.golang.org) is a separate
credentialed step; everything below is required **before** publish.

Related: the [SDK roadmap](sdk-roadmap.md), the per-SDK `PACKAGING.md` under `sdk/*/`, and the
root `VERSION`.

## Gates

Every gate must be green.

| Gate | Command |
| --- | --- |
| Version lockstep | `./scripts/check-sdk-versions.sh` |
| Fixture sync | `./scripts/sync-sdk-fixtures.sh` |
| Python package | `./scripts/package-python-client.sh` |
| Perl package | `./scripts/package-perl-client.sh` |
| Go package | `./scripts/package-go-client.sh` |
| Ruby package | `./scripts/package-ruby-client.sh` |
| Erlang package | `./scripts/package-erlang-client.sh` (requires OTP + rebar3) |
| C++ CMake client | `./scripts/verify-cpp-client-package.sh` |
| All of the above + checksums | `./scripts/package-all-sdk-clients.sh` |

CI job `sdk-clients` runs the version lock, language tests and package scripts for Python, Perl, Go,
Ruby and Erlang. The Python package script installs its wheel and normalized sdist into separate
virtual environments; the Perl package script installs its normalized tarball into an isolated
prefix; the Ruby package script installs its gem under a clean `GEM_HOME`. Each runs its complete
suite from a separate test tree, proving that the installed modules, not the source checkout,
satisfy conformance. Tar normalization suppresses macOS AppleDouble and xattr members, so a
nominally reproducible sdist keeps its single-root installable layout.

Go has no registry archive in this workflow: its packaging gate reconstructs the nested module from
tracked files, reruns the tests there and builds an external module consumer matching the VCS-tag
distribution model.

The `install-consumer` job covers CMake install and external consumer smokes; it requires OpenSSL
when the tree was built with TLS, and `FindGlifiStoreTls.cmake` is installed next to the package
config. The installed secure-profile matrix also uses `scripts/build-installed-cpp-interop.sh` to
build its C++ peer strictly through `GlifiStore::client` from an isolated installation prefix.

## Version policy

1. The root [`VERSION`](../../VERSION) is the canonical release number for all official SDKs while
   they stay in lockstep (current: `0.1.0`).
2. Language sources:
   - Python: `glifistore.__version__`
   - Perl: `our $VERSION` in every `lib/**/*.pm` (must be identical)
   - Go: `client.Version`
   - Ruby: `GlifiStore::VERSION`
   - Erlang: `glifistore_version:version/0`
   - C++: CMake `PROJECT_VERSION` from the root `VERSION`
3. Diverging an SDK version requires an ADR; until then CI fails on drift.

## Release-day artifacts

1. Run `./scripts/package-all-sdk-clients.sh`, which exports `SOURCE_DATE_EPOCH` from HEAD.
2. Optionally run `./scripts/verify-sdk-artifact-reproducibility.sh`, a two-pass digest comparison
   for wheels, gems and normalized Python, Perl, Go and Erlang source archives. Supply-chain CI also
   rebuilds on `ubuntu-22.04` and runs `./scripts/compare-sdk-artifact-sums.sh` against the primary
   sums.
3. Attach `dist/sdk-artifacts/SHA256SUMS` and `*.spdx.json` from the supply-chain workflow, or from
   `SYFT_REQUIRED=1 ./scripts/checksum-sdk-artifacts.sh`, to the GitHub Release. Python and Perl
   sdist names are prefixed `python-` and `perl-` in that directory, so they stay distinct on
   case-insensitive filesystems. The generated `sdk-release-index.json` binds every package name and
   digest to its SDK role and wire/client contract; it is an artifact catalog, not evidence that two
   different releases interoperate.
4. On tagged releases, the supply-chain workflow keyless-signs blobs with Cosign/Sigstore
   (`.cosign.bundle` next to each artifact and `SHA256SUMS`). An optional project GPG signature
   stays operator-owned.
5. Publish:
   - Python: `twine upload` (Trusted Publisher preferred)
   - Perl: PAUSE upload of `GlifiStore-VERSION.tar.gz`
   - Ruby: `gem push` (MFA required; `allowed_push_host=rubygems.org`)
   - Erlang: `rebar3 hex publish` when Hex credentials are configured
   - Go: `git tag sdk/go/vVERSION && git push origin sdk/go/vVERSION`
   - C++: ship the CMake installable prefix or source tag; vcpkg/Conan remain optional future work

## Operator-owned credentials

Automated publish workflows, production signing keys and registry account recovery are not stored
in this repository. The packaging gates above are complete without them.
