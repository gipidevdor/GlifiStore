#!/usr/bin/env bash
# Install runtimes required by scripts/test-package-installed-sdk-matrix.sh (plain).
#
# Called inside the digest-pinned packaging container after the package is
# installed. Missing toolchains produce an honest harness NOT_RUN/FAIL; this
# script only removes the "no ruby/erlang/go on the image" residual.
set -euo pipefail

log() { printf '%s\n' "$*"; }

ruby_is_new_enough() {
  local ruby_bin="${1:-ruby}"
  command -v "$ruby_bin" >/dev/null 2>&1 || return 1
  "$ruby_bin" -e \
    'v=RUBY_VERSION.split(".").map!(&:to_i); exit(v[0] > 3 || (v[0] == 3 && v[1] >= 2) ? 0 : 1)' \
    >/dev/null 2>&1
}

install_go_toolchain() {
  local need_major=1 need_minor=27
  local have_major=0 have_minor=0
  if command -v go >/dev/null 2>&1; then
    local version
    version="$(go env GOVERSION 2>/dev/null || go version | awk '{print $3}')"
    version="${version#go}"
    have_major="${version%%.*}"
    have_minor="${version#*.}"
    have_minor="${have_minor%%.*}"
  fi
  if [[ "$have_major" -gt "$need_major" ]] || {
    [[ "$have_major" -eq "$need_major" && "$have_minor" -ge "$need_minor" ]]
  }; then
    log "go $(go version) already satisfies >= ${need_major}.${need_minor}"
    return 0
  fi

  local arch goarch
  arch="$(uname -m)"
  case "$arch" in
    x86_64 | amd64) goarch=amd64 ;;
    aarch64 | arm64) goarch=arm64 ;;
    *)
      log "error: unsupported architecture for Go bootstrap: $arch"
      return 1
      ;;
  esac
  local version="1.27.1"
  local archive="go${version}.linux-${goarch}.tar.gz"
  local url="https://go.dev/dl/${archive}"
  local expect_sha
  case "$goarch" in
    amd64) expect_sha="63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445" ;;
    arm64) expect_sha="3450b45a3f9ee8568792736a5c5e70a1f2e9b36c35a8f74958c03e51d7d92bec" ;;
  esac
  local work
  work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-go-boot.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -rf '$work'" RETURN
  log "installing Go ${version} from ${url}"
  curl -fsSL "$url" -o "$work/$archive"
  echo "${expect_sha}  $work/$archive" | sha256sum -c -
  rm -rf /usr/local/go
  tar -C /usr/local -xzf "$work/$archive"
  ln -sfn /usr/local/go/bin/go /usr/local/bin/go
  ln -sfn /usr/local/go/bin/gofmt /usr/local/bin/gofmt
  go version
}

install_ruby_toolchain() {
  if ruby_is_new_enough ruby; then
    log "ruby $(ruby -v) already satisfies >= 3.2"
    return 0
  fi
  log "system ruby is too old or missing; installing Ruby 3.4 via mise"
  if [[ ! -x /usr/local/bin/mise ]]; then
    local arch mise_arch expect_sha
    arch="$(uname -m)"
    case "$arch" in
      x86_64 | amd64)
        mise_arch=x64
        expect_sha="d71e94e1ed59d4d0ca4ac847fa321d6d6615a8e613e9b468c9fb39f0dddd06d5"
        ;;
      aarch64 | arm64)
        mise_arch=arm64
        expect_sha="3a52c7c7c58d21a0791516950ebf4bc915f403277b49c93d657fc585259625ec"
        ;;
      *)
        log "error: unsupported architecture for mise bootstrap: $arch"
        return 1
        ;;
    esac
    local version="2026.9.5"
    local archive="mise-v${version}-linux-${mise_arch}.tar.gz"
    local url="https://github.com/jdx/mise/releases/download/v${version}/${archive}"
    local work
    work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-mise-boot.XXXXXX")"
    log "installing mise ${version} from ${url}"
    curl -fsSL "$url" -o "$work/$archive"
    echo "${expect_sha}  $work/$archive" | sha256sum -c -
    tar -C "$work" -xzf "$work/$archive"
    install -m 0755 "$work/mise/bin/mise" /usr/local/bin/mise
    rm -rf "$work"
  fi
  export MISE_YES=1
  /usr/local/bin/mise install ruby@3.4
  local prefix
  prefix="$(/usr/local/bin/mise where ruby@3.4)"
  ln -sfn "$prefix/bin/ruby" /usr/local/bin/ruby
  ln -sfn "$prefix/bin/gem" /usr/local/bin/gem
  ruby_is_new_enough /usr/local/bin/ruby
  ruby -v
}

if command -v apt-get >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    g++ \
    make \
    lsof \
    perl \
    python3 \
    python3-pip \
    python3-venv \
    erlang \
    rebar3
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y \
    ca-certificates \
    curl \
    gcc-c++ \
    make \
    lsof \
    perl \
    python3 \
    python3-pip \
    ruby \
    rubygems \
    erlang \
    rebar3
else
  log "error: neither apt-get nor dnf is available to install matrix toolchains"
  exit 1
fi

install_go_toolchain
install_ruby_toolchain

command -v python3 >/dev/null
command -v perl >/dev/null
command -v ruby >/dev/null
command -v go >/dev/null
command -v erl >/dev/null
command -v rebar3 >/dev/null
ruby_is_new_enough ruby || {
  log "error: ruby >= 3.2 is required for the installed SDK matrix"
  ruby -v || true
  exit 1
}

log "installed SDK matrix toolchains are ready"
