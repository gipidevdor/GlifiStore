#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "verify-xcode.sh must run on macOS" >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project="$root/build/xcode/GlifiStore.xcodeproj"

"$root/scripts/generate-xcode.sh" >/dev/null

build_target() {
    local configuration="$1"
    xcodebuild \
        -project "$project" \
        -target ALL_BUILD \
        -configuration "$configuration" \
        -destination "platform=macOS" \
        CODE_SIGNING_ALLOWED=NO \
        build
}

build_target Debug
build_target Release

xcodebuild \
    -project "$project" \
    -scheme check \
    -configuration Debug \
    -destination "platform=macOS" \
    CODE_SIGNING_ALLOWED=NO \
    build

"$root/build/xcode/Debug/glifistore_demo"

cat <<EOF
Xcode verification passed.

Project: $project
Configurations: Debug, Release
Tests: check scheme / CTest
EOF
