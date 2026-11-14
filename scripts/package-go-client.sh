#!/usr/bin/env bash
# Verify Go SDK packaging readiness (fixtures, tests, builds, version lock).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
go_bin="${GO:-go}"
sdk="$root/sdk/go"

for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$sdk/testdata/$fixture"; then
    echo "vendored fixture drift: $fixture" >&2
    echo "run ./scripts/sync-sdk-fixtures.sh" >&2
    exit 1
  fi
done

expected="$(tr -d '[:space:]' <"$root/VERSION")"
got="$("$go_bin" -C "$sdk" run ./cmd/glifistore-version)"
if [[ "$got" != "$expected" ]]; then
  echo "go client.Version='$got' does not match VERSION='$expected'" >&2
  exit 1
fi

(
  cd "$sdk"
  "$go_bin" test ./...
  "$go_bin" test -race ./client ./protocol
  mkdir -p bin
  "$go_bin" build -o bin/glifistore-interop ./cmd/glifistore-interop
  "$go_bin" build -o bin/glifistore-bench ./cmd/glifistore-bench
  "$go_bin" build -o bin/glifistore-version ./cmd/glifistore-version
  # Go ≥ 1.21 provides `mod tidy -diff`; the SDK floor is 1.27.
  if ! "$go_bin" mod tidy -diff; then
    echo "go.mod not tidy" >&2
    exit 1
  fi
)

# Go consumers receive the nested module from a VCS tag. Reconstruct that tag-shaped source from
# tracked files only, then test it and compile an external module against its public packages.
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-go-pack.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
module_snapshot="$work/glifistore-go-$got"
consumer="$work/consumer"
mkdir -p "$module_snapshot" "$consumer"
while IFS= read -r -d '' path; do
  relative="${path#sdk/go/}"
  mkdir -p "$module_snapshot/$(dirname "$relative")"
  cp "$root/$path" "$module_snapshot/$relative"
done < <(git -C "$root" ls-files -z -- sdk/go)
(
  cd "$module_snapshot"
  "$go_bin" test ./...
)
cat >"$consumer/go.mod" <<EOF
module glifistore-package-consumer

go 1.27

require github.com/gpicchiarelli/GlifiStore/sdk/go v0.0.0

replace github.com/gpicchiarelli/GlifiStore/sdk/go => ../$(basename "$module_snapshot")
EOF
cat >"$consumer/main.go" <<'EOF'
package main

import (
	"fmt"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

func main() {
	config := client.DefaultConfig()
	owner, err := protocol.WorkerFor([]byte("consumer-key"), 4)
	if err != nil || config.Host == "" {
		panic("invalid installed Go module")
	}
	fmt.Printf("glifistore=%s owner=%d\n", client.Version, owner)
}
EOF
(
  cd "$consumer"
  "$go_bin" build ./...
  output="$("$go_bin" run .)"
  [[ "$output" == "glifistore=$got owner="* ]] || {
    echo "unexpected Go consumer output: $output" >&2
    exit 1
  }
)

mkdir -p "$sdk/dist"
archive_name="glifistore-go-$got.tar.gz"
rm -f "$sdk/dist"/glifistore-go-*.tar.gz
tar -czf "$sdk/dist/$archive_name" -C "$work" "$(basename "$module_snapshot")"
"$root/scripts/normalize-tar-gz.sh" "$sdk/dist/$archive_name"

artifact_root="$work/artifact"
mkdir -p "$artifact_root"
tar -xzf "$sdk/dist/$archive_name" -C "$artifact_root"
(
  cd "$artifact_root/$(basename "$module_snapshot")"
  "$go_bin" test ./...
  "$go_bin" build -o "$work/glifistore-interop-artifact" ./cmd/glifistore-interop
)
if [[ ! -x "$work/glifistore-interop-artifact" ]]; then
  echo "Go source archive did not produce glifistore-interop" >&2
  exit 1
fi

{
  echo "module=github.com/gpicchiarelli/GlifiStore/sdk/go"
  echo "version=$got"
  echo "tag_hint=sdk/go/v$got"
  echo "go=$("$go_bin" version)"
  echo "source_date_epoch=$SOURCE_DATE_EPOCH"
  echo "built_at=$(glifistore_repro_iso8601)"
  echo "tracked_source_snapshot=tested"
  echo "external_module_consumer=passed"
  echo "source_archive=$archive_name"
  echo "external_archive_build=passed"
} >"$sdk/dist/package-info.txt"

for required in LICENSE NOTICE; do
  if [[ ! -f "$sdk/$required" ]]; then
    echo "ERROR: Go SDK missing $required" >&2
    exit 1
  fi
done

echo "Go packaging verification OK ($sdk/dist/$archive_name)"
echo "Publish path: git tag sdk/go/v$got && git push origin sdk/go/v$got"
