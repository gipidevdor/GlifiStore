#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project="$root/build/xcode/GlifiStore.xcodeproj"

"$root/scripts/generate-xcode.sh" >/dev/null

open "$project"
