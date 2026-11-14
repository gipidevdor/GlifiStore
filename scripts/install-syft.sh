#!/usr/bin/env bash
# Install a checksum-pinned Syft release binary into /usr/local/bin (or DESTDIR).
# Syft (Anchore) is Apache-2.0; see THIRD_PARTY_NOTICES.md. This script downloads
# an upstream release artifact — it does not relicense Syft.
# Override SYFT_VERSION / SYFT_SHA256 when bumping; keep supply-chain.yml in sync.
set -euo pipefail

SYFT_VERSION="${SYFT_VERSION:-v1.51.1}"
DEST_DIR="${DESTDIR:-/usr/local/bin}"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64 | amd64)
    SYFT_ARCH="amd64"
    DEFAULT_SHA="8fcb33017a0dc1058298c923c436d19dfa68ae93968e0b423248542e3afb9fc3"
    ;;
  aarch64 | arm64)
    SYFT_ARCH="arm64"
    DEFAULT_SHA="a7fd2b784e6664acd44719270574f6cd8c6864fc2b1700bf9099bd1cccda7d7f"
    ;;
  *)
    echo "unsupported architecture for Syft install: $ARCH" >&2
    exit 1
    ;;
esac
SYFT_SHA256="${SYFT_SHA256:-$DEFAULT_SHA}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
archive="syft_${SYFT_VERSION#v}_linux_${SYFT_ARCH}.tar.gz"
url="https://github.com/anchore/syft/releases/download/${SYFT_VERSION}/${archive}"
curl -sSfL -o "$workdir/$archive" "$url"
echo "${SYFT_SHA256}  $workdir/$archive" | sha256sum -c -
tar -xzf "$workdir/$archive" -C "$workdir" syft
install -m 0755 "$workdir/syft" "$DEST_DIR/syft"
syft version
