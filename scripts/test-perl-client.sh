#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
perl="${PERL:-perl}"
sdk="$root/sdk/perl"

cd "$root"

if ! "$perl" -MPerl::Critic -e1 >/dev/null 2>&1; then
  echo "Perl::Critic is required (severity 1 / brutal). Install with: cpanm Perl::Critic Perl::Tidy" >&2
  exit 1
fi
if ! "$perl" -MPerl::Tidy -e1 >/dev/null 2>&1; then
  echo "Perl::Tidy is required by .perlcriticrc RequireTidyCode. Install with: cpanm Perl::Tidy" >&2
  exit 1
fi

echo "Perl::Critic (brutal / severity 1) on $sdk/lib"
(
  cd "$sdk"
  "$perl" -S perlcritic lib/
)

PERL5LIB="$sdk/lib${PERL5LIB:+:$PERL5LIB}" "$perl" -MTest::Harness -e \
  'runtests(@ARGV)' "$sdk/t/01-protocol.t" "$sdk/t/02-client.t" "$sdk/t/03-error-taxonomy.t"
