#!/usr/bin/env bash
# Remove generator-version drift from META files before tar metadata normalization.
# Usage: normalize-perl-dist-metadata.sh <distribution.tar.gz>
set -euo pipefail

if [[ $# -ne 1 || ! -f "$1" ]]; then
  echo "usage: $0 <distribution.tar.gz>" >&2
  exit 2
fi

archive="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
perl_bin="${PERL:-perl}"
work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-norm-perl.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$work/src"
tar -xzf "$archive" -C "$work/src"

roots=("$work/src"/*)
if [[ ${#roots[@]} -ne 1 || ! -d "${roots[0]}" ]]; then
  echo "normalize-perl-dist-metadata: expected one distribution root" >&2
  exit 1
fi
meta_json="${roots[0]}/META.json"
meta_yaml="${roots[0]}/META.yml"
if [[ ! -f "$meta_json" || ! -f "$meta_yaml" ]]; then
  echo "normalize-perl-dist-metadata: META.json and META.yml are required" >&2
  exit 1
fi

"$perl_bin" -0pi -e '
  $generated = s/"generated_by"\s*:\s*"[^"]*"/"generated_by" : "ExtUtils::MakeMaker, canonicalized by GlifiStore"/;
  $backend = s/"x_serialization_backend"\s*:\s*"[^"]*"/"x_serialization_backend" : "JSON::PP, canonicalized by GlifiStore"/;
  END { die "META.json generator fields missing\n" unless $generated == 1 && $backend == 1; }
' "$meta_json"
"$perl_bin" -0pi -e '
  $generated = s/^generated_by:.*$/generated_by: '\''ExtUtils::MakeMaker, canonicalized by GlifiStore'\''/m;
  $backend = s/^x_serialization_backend:.*$/x_serialization_backend: '\''CPAN::Meta::YAML, canonicalized by GlifiStore'\''/m;
  END { die "META.yml generator fields missing\n" unless $generated == 1 && $backend == 1; }
' "$meta_yaml"

out="$work/out.tar.gz"
(
  cd "$work/src"
  tar -czf "$out" "$(basename "${roots[0]}")"
)
mv -f "$out" "$archive"
echo "canonicalized Perl distribution metadata in $archive"
