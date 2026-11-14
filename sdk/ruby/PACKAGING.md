# Ruby client packaging

How maintainers verify, build and publish the `glifistore` gem, module `GlifiStore`, to RubyGems. It
needs Ruby 3.2 or later, its license is BSD-3-Clause, and it has no runtime dependency outside the
standard library; the optional async stack, the `async` gem, loads only through
`require "glifi_store/async_client"`.

## Verify

From the repository root:

```bash
./scripts/package-ruby-client.sh
```

The script:

1. confirms that the vendored wire fixtures match the repository corpus;
2. asserts that `GlifiStore::VERSION` matches `VERSION`;
3. builds `sdk/ruby/dist/glifistore-VERSION.gem`;
4. installs the gem into a clean `GEM_HOME`;
5. runs the installed `glifistore-interop` executable with no source-tree load path;
6. runs the Minitest suite against the installed gem, plus the development gems `async` and
   `minitest`.

With the Python and Perl packages and the TLS daemon and client peers already built, the installed
gem can also run through the fail-closed secure-profile matrix:

```bash
RUBY=/path/to/ruby-3.2-or-newer \
PERL=/path/to/perl-with-TLS-1.3 \
GLIFISTORED=/path/to/glifistored \
GLIFISTORE_INTEROP_CLIENT=/path/to/glifistore_interop_client \
GLIFISTORE_GO_INTEROP=/path/to/glifistore-interop \
./scripts/test-secure-profile-installed-artifacts.sh
```

## Build

```bash
cd sdk/ruby
gem build glifistore.gemspec --output dist/glifistore-0.1.0.gem
```

## Publish

```bash
gem push sdk/ruby/dist/glifistore-0.1.0.gem
```

Pushing requires a RubyGems account with MFA, and `allowed_push_host` is restricted to
`https://rubygems.org`. Do not commit API keys.

## Bump the version

`GlifiStore::VERSION` in `lib/glifi_store/version.rb` is the single source. It must match the
repository root `VERSION` file, which `./scripts/check-sdk-versions.sh` checks.

1. Bump `lib/glifi_store/version.rb`.
2. Update `CHANGELOG.md`.
3. Make sure the root `VERSION` matches; the SDKs move in lockstep.
4. Run `./scripts/check-sdk-versions.sh` and `./scripts/package-ruby-client.sh`.
5. Tag, release and `gem push`.
