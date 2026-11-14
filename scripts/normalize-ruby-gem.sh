#!/usr/bin/env bash
# Canonicalize RubyGems' nested gzip/tar streams and outer tar metadata.
# Usage: normalize-ruby-gem.sh <artifact.gem>
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python="${PYTHON:-$(command -v python3 || true)}"

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <artifact.gem>" >&2
  exit 2
fi
if [[ -z "$python" || ! -x "$python" ]]; then
  echo "python3 is required to normalize Ruby gems" >&2
  exit 1
fi
exec "$python" "$root/scripts/normalize-ruby-gem.py" "$1"
