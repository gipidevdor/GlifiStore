#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "generate-xcode.sh must run on macOS" >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake="$root/.tools/venv/bin/cmake"
project="$root/build/xcode/GlifiStore.xcodeproj"
shared_schemes="$project/xcshareddata/xcschemes"

if ! xcodebuild -version >/dev/null 2>&1; then
    echo "Full Xcode is required. Select it with xcode-select before continuing." >&2
    exit 1
fi
if [[ ! -x "$cmake" ]]; then
    "$root/scripts/bootstrap-macos.sh"
    exit 0
fi

# CMake does not remove shared schemes for targets that stopped requesting one. They are generated
# state, so clear only that directory before refreshing the project; personal xcuserdata is kept.
rm -rf "$shared_schemes"
PATH="$root/.tools/venv/bin:$PATH" "$cmake" --preset xcode

if [[ ! -d "$project" ]]; then
    echo "CMake did not generate the expected Xcode project: $project" >&2
    exit 1
fi

echo "$project"
