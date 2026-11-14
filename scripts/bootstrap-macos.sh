#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "bootstrap-macos.sh must run on macOS" >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tools="$root/.tools"
venv="$tools/venv"

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode Command Line Tools are missing. Run: xcode-select --install" >&2
    exit 1
fi

if ! xcodebuild -version >/dev/null 2>&1; then
    echo "Full Xcode is required. Select it with: sudo xcode-select -s /Applications/Xcode.app" >&2
    exit 1
fi

mkdir -p "$tools"
if [[ ! -x "$venv/bin/python" ]]; then
    python3 -m venv "$venv"
fi

"$venv/bin/python" -m pip install --disable-pip-version-check --upgrade pip
formatter_version="$(<"$root/.clang-format-version")"
"$venv/bin/python" -m pip install --disable-pip-version-check \
    "cmake>=3.25,<5" "ninja>=1.11,<2" "clang-format==$formatter_version"

"$root/scripts/generate-xcode.sh" >/dev/null

cat <<EOF
GlifiStore macOS environment is ready.

Xcode:  $(xcodebuild -version | paste -sd ' ' -)
CMake:  $("$venv/bin/cmake" --version | head -1)
Ninja:  $("$venv/bin/ninja" --version)
Project: $root/build/xcode/GlifiStore.xcodeproj

Open it with:
  $root/scripts/open-xcode.sh
EOF
