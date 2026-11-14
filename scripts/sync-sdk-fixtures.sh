#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for fixture in wire_requests_v2.hex wire_responses_v2.hex error_taxonomy_v1.json; do
  cp "$root/tests/fixtures/$fixture" "$root/sdk/python/tests/fixtures/$fixture"
  cp "$root/tests/fixtures/$fixture" "$root/sdk/perl/t/fixtures/$fixture"
  cp "$root/tests/fixtures/$fixture" "$root/sdk/go/testdata/$fixture"
  cp "$root/tests/fixtures/$fixture" "$root/sdk/ruby/test/fixtures/$fixture"
  cp "$root/tests/fixtures/$fixture" "$root/sdk/erlang/test/fixtures/$fixture"
  echo "synced $fixture"
done
