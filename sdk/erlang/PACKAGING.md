# Erlang client packaging

How maintainers verify and publish `glifistore`, the OTP application under `sdk/erlang`, to Hex. Its
runtime dependencies are OTP 25 or later standard applications only (`kernel`, `stdlib`, `crypto`;
`ssl` optional, for TLS), and its license is BSD-3-Clause.

## Verify

From the repository root:

```bash
./scripts/package-erlang-client.sh
```

The script:

1. confirms that the vendored wire fixtures match the repository corpus;
2. runs `rebar3 compile` and `rebar3 ct`;
3. asserts that `glifistore_version:version/0` matches `VERSION`;
4. smoke-checks `scripts/glifistore-version.escript`;
5. builds a normalized `sdk/erlang/dist/glifistore-erlang-VERSION.tar.gz` from tracked files only;
6. extracts and compiles that archive outside the checkout, then verifies its runtime version;
7. writes `sdk/erlang/dist/package-info.txt`.

With the other SDK packages and the TLS daemon and client peers built, the extracted archive can
also run through the fail-closed secure-profile matrix:

```bash
GLIFISTORED=/path/to/glifistored \
GLIFISTORE_INTEROP_CLIENT=/path/to/glifistore_interop_client \
GLIFISTORE_GO_INTEROP=/path/to/glifistore-interop \
./scripts/test-secure-profile-installed-artifacts.sh
```

## Publish

Hex packages are the primary distribution. Once the package is on Hex, consumers depend on it:

```erlang
%% After Hex publish:
%% {deps, [{glifistore, "0.1.0"}]}.
{ok, Client} = glifistore_client:connect(#{host => "127.0.0.1", port => 7379}).
```

Until Hex publication is configured, consume the application from this repository with a `rebar3`
path dependency, or copy the OTP application into your release.

Until a registry publication exists, CI attaches the normalized tracked-source archive,
`dist/package-info.txt`, checksums and an SBOM through `scripts/checksum-sdk-artifacts.sh`. The
source archive is not described as a Hex package.

## Bump the version

`glifistore_version:version/0` is the single source of the Erlang client version. It must match the
repository root `VERSION` file, which `./scripts/check-sdk-versions.sh` enforces.

1. Bump `src/glifistore_version.erl` and `vsn` in `src/glifistore.app.src`.
2. Update `CHANGELOG.md`.
3. Make sure the root `VERSION` matches; all SDKs move in lockstep unless an ADR says otherwise.
4. Run `./scripts/check-sdk-versions.sh` and `./scripts/package-erlang-client.sh`.
5. Publish to Hex with `rebar3 hex publish` when credentials are available.
