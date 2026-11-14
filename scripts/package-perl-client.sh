#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GLIFISTORE_ROOT="$root"
# shellcheck disable=SC1091
source "$root/scripts/export-reproducible-build-env.sh"
perl="${PERL:-perl}"
make="${MAKE:-make}"
sdk="$root/sdk/perl"

for fixture in wire_requests_v2.hex wire_responses_v2.hex; do
  if ! cmp -s "$root/tests/fixtures/$fixture" "$sdk/t/fixtures/$fixture"; then
    echo "vendored fixture drift: $fixture" >&2
    echo "copy repository fixtures into sdk/perl/t/fixtures/ before packaging" >&2
    exit 1
  fi
done

expected="$(tr -d '[:space:]' <"$root/VERSION")"
# Prefer portable tools over ripgrep (not installed on all CI images).
perl_versions="$(
  find "$sdk/lib" -type f -name '*.pm' -print0 |
    xargs -0 "$perl" -ne "print \$1, \"\\n\" if /our \\\$VERSION = '([^']+)'/" |
    sort -u
)"
perl_count="$(printf '%s\n' "$perl_versions" | grep -c . || true)"
if [[ "$perl_count" -ne 1 || "$perl_versions" != "$expected" ]]; then
  echo "Perl \$VERSION drift or mismatch: '${perl_versions:-<none>}' (expected $expected)" >&2
  exit 1
fi
echo "Perl VERSION $perl_versions OK"

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-perl-pack.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Build from a clean copy so disttest does not fight the live tree.
cp -R "$sdk/." "$work/sdk"
cd "$work/sdk"
rm -rf blib GlifiStore-* Makefile Makefile.old pm_to_blib MYMETA.* *.tar.gz

"$perl" Makefile.PL
"$make" manifest
# ExtUtils::Manifest skips dotfiles by default; keep the brutal critic profile in the tarball.
for profile in .perlcriticrc .perltidyrc; do
  if [[ -f "$profile" ]] && ! grep -qxF "$profile" MANIFEST; then
    echo "$profile" >>MANIFEST
  fi
done
"$make" disttest
"$make" dist

tarball=(GlifiStore-*.tar.gz)
if [[ ! -f "${tarball[0]}" ]]; then
  echo "expected GlifiStore-*.tar.gz was not produced" >&2
  exit 1
fi

"$perl" -MJSON::PP=decode_json -e '
  open my $fh, "<:raw", "MYMETA.json" or die $!;
  local $/;
  my $meta = decode_json(<$fh>);
  die "license missing\n" unless ($meta->{license} // "") =~ /bsd/i
      || grep { /bsd/i } @{$meta->{license} // []};
  for my $module (qw(
    GlifiStore GlifiStore::Client GlifiStore::Protocol
    GlifiStore::Error GlifiStore::SendFailure
  )) {
    die "$module missing from provides\n" unless $meta->{provides}{$module};
  }
  print "META provides and license OK\n";
' 2>/dev/null || "$perl" -e '
  open my $fh, "<", "MYMETA.yml" or die $!;
  local $/;
  my $text = <$fh>;
  die "license missing\n" unless $text =~ /license:\s*.*bsd/i;
  for my $module (qw(
    GlifiStore GlifiStore::Client GlifiStore::Protocol
    GlifiStore::Error GlifiStore::SendFailure
  )) {
    die "$module missing from provides\n" unless $text =~ /\Q$module\E/;
  }
  print "META provides and license OK\n";
'

mkdir -p "$sdk/dist"
cp "${tarball[0]}" "$sdk/dist/"
"$root/scripts/normalize-perl-dist-metadata.sh" "$sdk/dist/$(basename "${tarball[0]}")"
"$root/scripts/normalize-tar-gz.sh" "$sdk/dist/$(basename "${tarball[0]}")"

# Dist tarball must retain LICENSE and NOTICE.
tar_tz="$sdk/dist/$(basename "${tarball[0]}")"
# Materialize the listing first: `tar | head` under `pipefail` fails with
# "tar: stdout: write error" when head closes the pipe early (SIGPIPE).
tar_list="$(tar -tzf "$tar_tz")"
prefix="$(printf '%s\n' "$tar_list" | head -1 | cut -d/ -f1)"
for required in LICENSE NOTICE; do
  if ! printf '%s\n' "$tar_list" | grep -qx "${prefix}/${required}"; then
    echo "ERROR: Perl dist missing $required" >&2
    exit 1
  fi
done

# Verify the normalized release artifact as an installed package. Tests run from a separate tree so
# FindBin cannot resolve the distribution's source lib/ directory and mask a broken installation.
artifact_root="$work/artifact"
install_root="$work/install"
test_root="$work/installed-test"
mkdir -p "$artifact_root" "$install_root" "$test_root"
tar -xzf "$tar_tz" -C "$artifact_root"
(
  cd "$artifact_root/$prefix"
  "$perl" Makefile.PL INSTALL_BASE="$install_root"
  "$make"
  "$make" install
)
installed_lib="$install_root/lib/perl5"
if [[ ! -f "$installed_lib/GlifiStore/Client.pm" ]]; then
  echo "ERROR: installed Perl package is missing GlifiStore::Client" >&2
  exit 1
fi
cp -R "$artifact_root/$prefix/t" "$test_root/t"
PERL5LIB="$installed_lib" "$perl" -e '
  use GlifiStore;
  my $root = shift;
  die "loaded GlifiStore from outside isolated install: $INC{q{GlifiStore.pm}}\n"
      if index($INC{q{GlifiStore.pm}}, $root) != 0;
' "$installed_lib"
PERL5LIB="$installed_lib" "$perl" -MTest::Harness -e \
  'runtests(@ARGV)' "$test_root/t/01-protocol.t" "$test_root/t/02-client.t" \
  "$test_root/t/03-error-taxonomy.t"
echo "Installed Perl artifact conformance OK ($installed_lib)"

echo "Perl packaging verification OK ($sdk/dist/$(basename "${tarball[0]}"))"
