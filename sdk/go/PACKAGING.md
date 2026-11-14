# Go client packaging

How maintainers verify and publish the Go module `github.com/gpicchiarelli/GlifiStore/sdk/go`. It
has no runtime dependency outside the standard library, and its license is BSD-3-Clause.

## Verify

From the repository root:

```bash
./scripts/package-go-client.sh
```

The script:

1. confirms that the vendored wire fixtures match the repository corpus;
2. asserts that `client.Version` matches `VERSION`;
3. runs `go test ./...` and the race tests on `client` and `protocol`;
4. builds `glifistore-interop`, `glifistore-bench` and `glifistore-version`;
5. checks `go mod tidy` cleanliness when supported;
6. reconstructs a tag-shaped module from tracked files only and reruns its tests;
7. compiles and runs an external consumer of the snapshot's public `client` and `protocol`
   packages;
8. produces a normalized `glifistore-go-VERSION.tar.gz` from that tracked snapshot;
9. extracts the archive and rebuilds the interop executable outside the checkout;
10. writes `sdk/go/dist/package-info.txt`.

A `sdk/go/vVERSION` tag is considered publishable after this gate has reconstructed the nested
module from tracked files only, tested that snapshot and built the external consumer.

## Publish

The Go module proxy fetches modules from git tags. A nested-module tag must have this form:

```text
sdk/go/v0.1.0
```

A repository-root `v0.1.0` tag alone is not enough: proxy.golang.org resolves the subdirectory
module through the `sdk/go/` prefix. Consumers then run:

```bash
go get github.com/gpicchiarelli/GlifiStore/sdk/go@v0.1.0
```

Optional CLI binaries may be attached to a GitHub Release with SHA-256 checksums (see
`scripts/checksum-sdk-artifacts.sh`). The normalized source archive is verification and release
evidence for the tag-shaped module; it does not replace the `sdk/go/vVERSION` tag that the Go module
proxy consumes.

## Bump the version

`client.Version` in `client/version.go` is the single source of the Go client version. It must match
the repository root `VERSION` file, which `./scripts/check-sdk-versions.sh` enforces.

1. Bump `Version` in `client/version.go`.
2. Update `CHANGELOG.md`.
3. Make sure the root `VERSION` matches; all SDKs move in lockstep unless an ADR says otherwise.
4. Run `./scripts/check-sdk-versions.sh` and `./scripts/package-go-client.sh`.
5. Tag `sdk/go/vX.Y.Z` and push the tag.
