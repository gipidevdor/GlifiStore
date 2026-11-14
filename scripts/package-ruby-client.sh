#!/usr/bin/env bash
# Build and verify the Ruby gem from a clean install path.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
sdk="$root/sdk/ruby"
ruby_bin="${RUBY:-}"
if [[ -z "$ruby_bin" && -x "$HOME/.local/bin/mise" ]]; then
  ruby_bin="$("$HOME/.local/bin/mise" which ruby@3.4 2>/dev/null || true)"
fi
ruby_bin="${ruby_bin:-$(command -v ruby || true)}"
if [[ -z "$ruby_bin" || ! -x "$ruby_bin" ]]; then
  echo "ruby >= 3.2 required for packaging" >&2
  exit 1
fi
gem_bin="${GEM:-$("$ruby_bin" -e 'print Gem.default_bindir + "/gem"')}"
if [[ ! -x "$gem_bin" ]]; then
  gem_bin="$(dirname "$ruby_bin")/gem"
fi

for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$sdk/test/fixtures/$fixture"; then
    echo "vendored fixture drift: $fixture" >&2
    echo "run ./scripts/sync-sdk-fixtures.sh" >&2
    exit 1
  fi
done

expected="$(tr -d '[:space:]' <"$root/VERSION")"
got="$(RUBYLIB="$sdk/lib" "$ruby_bin" -e 'require "glifi_store/version"; print GlifiStore::VERSION')"
if [[ "$got" != "$expected" ]]; then
  echo "GlifiStore::VERSION='$got' does not match VERSION='$expected'" >&2
  exit 1
fi
if ! "$ruby_bin" -e 'v=RUBY_VERSION.split(".").map!(&:to_i); exit(v[0] > 3 || (v[0]==3 && v[1] >= 2) ? 0 : 1)'; then
  echo "ruby >= 3.2 required to package gem (got $($ruby_bin -v))" >&2
  exit 1
fi

rm -rf "$sdk/dist" "$sdk"/glifistore-*.gem
mkdir -p "$sdk/dist"
(
  cd "$sdk"
  "$gem_bin" build glifistore.gemspec
  mv -f "glifistore-$got.gem" "dist/glifistore-$got.gem"
)
"$root/scripts/normalize-ruby-gem.sh" "$sdk/dist/glifistore-$got.gem"
"$gem_bin" specification "$sdk/dist/glifistore-$got.gem" >/dev/null

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-ruby-pack.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Gem must ship copyright notices (BSD-3-Clause redistribution).
"$gem_bin" unpack --target "$work" "$sdk/dist/glifistore-$got.gem" >/dev/null
gem_dir="$work/glifistore-$got"
for required in LICENSE NOTICE; do
  if [[ ! -f "$gem_dir/$required" ]]; then
    echo "ERROR: gem missing $required" >&2
    exit 1
  fi
done

export GEM_HOME="$work/gem_home"
export GEM_PATH="$GEM_HOME"
export PATH="$GEM_HOME/bin:$PATH"
mkdir -p "$GEM_HOME"
"$gem_bin" install --local --no-document "$sdk/dist/glifistore-$got.gem"
"$gem_bin" install --no-document async minitest >/dev/null

installed_cli="$GEM_HOME/bin/glifistore-interop"
if [[ ! -x "$installed_cli" ]]; then
  echo "ERROR: installed gem is missing glifistore-interop" >&2
  exit 1
fi
(
  cd "$work"
  unset RUBYLIB || true
  installed_help="$("$installed_cli" --help)"
  grep -q '^Usage: glifistore-interop ' <<<"$installed_help"
)
echo "Installed Ruby interop CLI load-path OK ($installed_cli)"

# Run the test suite against the *installed* gem (not the tree lib/).
cp -R "$sdk/test" "$work/test"
(
  cd "$work"
  unset RUBYLIB || true
  "$ruby_bin" -Itest -e 'Dir["test/test_*.rb"].sort.each { |f| require "./#{f}" }'
)

echo "Ruby packaging verification OK ($sdk/dist/glifistore-$got.gem)"
