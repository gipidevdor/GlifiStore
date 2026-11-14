#!/usr/bin/env bash
# Run GlifiStore Ruby SDK tests (requires Ruby >= 3.2).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ruby_bin="${RUBY:-}"
if [[ -z "$ruby_bin" ]]; then
  if command -v mise >/dev/null 2>&1; then
    ruby_bin="$(mise exec ruby@3.4 -- which ruby 2>/dev/null || true)"
  fi
fi
if [[ -z "$ruby_bin" ]]; then
  ruby_bin="$(command -v ruby)"
fi

version="$("$ruby_bin" -e 'print RUBY_VERSION')"
major="${version%%.*}"
minor_patch="${version#*.}"
minor="${minor_patch%%.*}"
if (( major < 3 || (major == 3 && minor < 2) )); then
  echo "Ruby >= 3.2 required (found $version via $ruby_bin)" >&2
  exit 1
fi

export RUBYLIB="$root/sdk/ruby/lib${RUBYLIB:+:$RUBYLIB}"
cd "$root/sdk/ruby"
"$ruby_bin" -Ilib:test -e 'Dir["test/test_*.rb"].sort.each { |f| require "./#{f}" }'
echo "Ruby SDK tests PASSED ($version)"
