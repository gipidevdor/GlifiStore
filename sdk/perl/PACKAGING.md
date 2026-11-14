# Perl client packaging

How maintainers verify, build and publish `GlifiStore`, the self-contained Perl distribution in this
directory, to CPAN through PAUSE and MetaCPAN. Its runtime dependencies are core modules only, on
Perl 5.32 or later.

## Requirements

- Perl 5.32 or later, with ExtUtils::MakeMaker 7.12 or later.
- Test::More 0.98 or later, usually bundled with Test-Simple.
- Canonical wire fixtures vendored under `t/fixtures/`, matching the repository
  `tests/fixtures/wire_*_v2.hex`. After changing the repository corpus, refresh them with
  `./scripts/sync-sdk-fixtures.sh`.
- The BSD-3-Clause `LICENSE`, matching the GlifiStore project.
- A PAUSE account authorized to upload the `GlifiStore` namespace.
- For the local quality gates, `Perl::Critic` and `Perl::Tidy`, the develop requirements in
  `cpanfile`. `./scripts/test-perl-client.sh` runs critic at severity 1 before the tests.

## Verify

From the repository root:

```bash
./scripts/package-perl-client.sh
```

The script:

1. confirms that the vendored fixtures match the repository corpus;
2. regenerates `MANIFEST`;
3. runs `make disttest`, which configures, builds and tests from the tarball;
4. builds `GlifiStore-VERSION.tar.gz`;
5. checks that `META.json` declares the expected provides and the BSD license;
6. normalizes and extracts the final tarball, installs it under an isolated prefix and reruns every
   test from a separate directory that cannot resolve the distribution's source `lib/`, so local
   `lib/` files cannot mask a packaging defect.

With the packaged Python peer and the TLS daemon and client helpers already built, the installed
Perl tarball can also run through the fail-closed secure-profile matrix:

```bash
PERL=/path/to/perl-with-IO-Socket-SSL \
GLIFISTORED=/path/to/glifistored \
GLIFISTORE_INTEROP_CLIENT=/path/to/glifistore_interop_client \
GLIFISTORE_GO_INTEROP=/path/to/glifistore-interop \
./scripts/test-secure-profile-installed-artifacts.sh
```

## Build

```bash
cd sdk/perl
perl Makefile.PL
make manifest
make disttest
make dist
```

## Publish

1. Make sure `$VERSION` is identical in `lib/GlifiStore.pm`, `lib/GlifiStore/Client.pm`,
   `lib/GlifiStore/Protocol.pm` and `Changes`.
2. Rerun `./scripts/package-perl-client.sh`.
3. Upload `GlifiStore-0.1.0.tar.gz` through [PAUSE](https://pause.perl.org/), with "Upload a file to
   CPAN".
4. Confirm the indexing on [MetaCPAN](https://metacpan.org/pod/GlifiStore).

To check the tarball with a local install:

```bash
cpanm ./GlifiStore-0.1.0.tar.gz
```

## Bump the version

1. Bump `$VERSION` in every `.pm` under `lib/`; the values must stay identical.
2. Update the root `VERSION` and every other official SDK in lockstep, or land an ADR.
3. Update `Changes`.
4. Keep the `Makefile.PL` `provides` versions in sync, from the same scalar.
5. Run `./scripts/check-sdk-versions.sh` and `./scripts/package-perl-client.sh`.
6. Upload the new tarball to PAUSE.
