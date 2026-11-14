#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python="${PYTHON:-python3}"

cd "$root/sdk/python"
PYTHONPATH=src PYTHONWARNINGS="error::ResourceWarning${PYTHONWARNINGS:+,$PYTHONWARNINGS}" \
  "$python" -m unittest discover -s tests -v
