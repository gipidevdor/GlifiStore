#!/usr/bin/env bash
# Live HEALTH/READY/STATS interop against a real glifistored.
# Complements fake-server unit coverage in each language SDK.
# Soft-skips languages whose toolchain is absent unless PROBE_INTEROP_REQUIRE_ALL=1.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python="${PYTHON:-python3}"
perl="${PERL:-perl}"
prefer_bins=(
  "$root/build/macos-debug"
  "$root/build/macos-release"
  "$root/build/macos-native-release"
  "$root/build/unix-release"
  "$root/build/unix-debug"
  "$root/build/macos-ci"
)

resolve_bin() {
  local name="$1"
  local override="$2"
  if [[ -n "$override" && -x "$override" ]]; then
    printf '%s\n' "$override"
    return 0
  fi
  for dir in "${prefer_bins[@]}"; do
    if [[ -x "$dir/$name" ]]; then
      printf '%s\n' "$dir/$name"
      return 0
    fi
  done
  return 1
}

discover_port() {
  local pid="$1"
  lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null |
    awk 'NR==2 {split($9,a,":"); print a[length(a)]}'
}

daemon="$(resolve_bin glifistored "${GLIFISTORED:-}" || true)"
if [[ -z "$daemon" || ! -x "$daemon" ]]; then
  echo "missing glifistored; build a preset that produces it first" >&2
  exit 1
fi
cpp_client="$(resolve_bin glifistore_interop_client "${GLIFISTORE_INTEROP_CLIENT:-}" || true)"
if ! command -v lsof >/dev/null 2>&1; then
  echo "lsof is required to discover ephemeral glifistored ports" >&2
  exit 1
fi

go_helper="${GLIFISTORE_GO_INTEROP:-}"
if [[ -z "$go_helper" || ! -x "$go_helper" ]]; then
  if command -v "${GO:-go}" >/dev/null 2>&1; then
    mkdir -p "$root/sdk/go/bin"
    (cd "$root/sdk/go" && "${GO:-go}" build -o bin/glifistore-interop ./cmd/glifistore-interop)
    go_helper="$root/sdk/go/bin/glifistore-interop"
  fi
fi

export PYTHONPATH="$root/sdk/python/src${PYTHONPATH:+:$PYTHONPATH}"
export PERL5LIB="$root/sdk/perl/lib${PERL5LIB:+:$PERL5LIB}"

ruby_bin="${RUBY:-}"
ruby_ready=0
if [[ -z "$ruby_bin" ]] && command -v ruby >/dev/null 2>&1; then
  ruby_bin="$(command -v ruby)"
fi
if [[ -n "$ruby_bin" && -x "$ruby_bin" ]]; then
  export RUBYLIB="$root/sdk/ruby/lib${RUBYLIB:+:$RUBYLIB}"
  ruby_ready=1
fi

erlang_ready=0
if command -v erl >/dev/null 2>&1 && command -v rebar3 >/dev/null 2>&1; then
  erlang_ready=1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-probe-interop.XXXXXX")"
daemon_pid=""
cleanup() {
  if [[ -n "${daemon_pid:-}" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

log_out="$work/daemon.out"
log_err="$work/daemon.err"

"$daemon" --quiet --bind 127.0.0.1 --port 0 --shard-pairs 1 \
  --storage-mode volatile --shutdown-drain-ms 2000 \
  >"$log_out" 2>"$log_err" &
daemon_pid=$!

port=""
for _ in $(seq 1 100); do
  port="$(discover_port "$daemon_pid" || true)"
  if [[ -n "$port" ]]; then
    break
  fi
  if ! kill -0 "$daemon_pid" 2>/dev/null; then
    echo "daemon exited early:" >&2
    cat "$log_out" "$log_err" >&2 || true
    exit 1
  fi
  sleep 0.05
done
if [[ -z "$port" ]]; then
  echo "failed to discover daemon port" >&2
  cat "$log_out" "$log_err" >&2 || true
  exit 1
fi

require_all="${PROBE_INTEROP_REQUIRE_ALL:-0}"
failures=0
ran=0

expect_probes() {
  local label="$1"
  local health="$2"
  local ready="$3"
  local stats="$4"
  if [[ "$health" != "GlifiStore/live" ]]; then
    echo "FAIL $label health: expected GlifiStore/live, got: $health" >&2
    return 1
  fi
  if [[ "$ready" != "GlifiStore/ready" ]]; then
    echo "FAIL $label ready: expected GlifiStore/ready, got: $ready" >&2
    return 1
  fi
  if [[ "$stats" != GlifiStore/stats* ]]; then
    echo "FAIL $label stats: expected GlifiStore/stats prefix, got: $stats" >&2
    return 1
  fi
  echo "ok: $label HEALTH/READY/STATS"
}

run_python() {
  local out
  out="$("$python" - <<PY
from glifistore.client import Client, ClientConfig
c = Client.connect(ClientConfig(host="127.0.0.1", port=$port))
try:
    print(c.health().decode())
    print("---")
    print(c.ready().decode())
    print("---")
    print(c.stats().decode())
finally:
    c.close()
PY
)"
  local health ready stats
  health="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==1{gsub(/\n$/,""); print}')"
  ready="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==2{gsub(/\n$/,""); print}')"
  stats="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==3{print}')"
  expect_probes "python" "$health" "$ready" "$stats"
}

run_go() {
  if [[ -z "$go_helper" || ! -x "$go_helper" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Go interop helper" >&2
      return 1
    fi
    echo "note: Go probe interop soft-skipped (no go helper)" >&2
    return 0
  fi
  local out health ready stats
  out="$("$go_helper" --host 127.0.0.1 --port "$port" health)"
  health="$(printf '%s' "$out")"
  ready="$("$go_helper" --host 127.0.0.1 --port "$port" ready)"
  stats="$("$go_helper" --host 127.0.0.1 --port "$port" stats)"
  expect_probes "go" "$health" "$ready" "$stats"
}

run_cpp() {
  if [[ -z "$cpp_client" || ! -x "$cpp_client" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing C++ interop helper" >&2
      return 1
    fi
    echo "note: C++ probe interop soft-skipped (no interop client)" >&2
    return 0
  fi
  local health ready stats
  health="$("$cpp_client" --host 127.0.0.1 --port "$port" health)"
  ready="$("$cpp_client" --host 127.0.0.1 --port "$port" ready)"
  stats="$("$cpp_client" --host 127.0.0.1 --port "$port" stats)"
  expect_probes "cpp" "$health" "$ready" "$stats"
}

run_perl() {
  local out health ready stats
  out="$(
    GLIFI_PROBE_PORT="$port" "$perl" - <<'PERL'
use strict;
use warnings;
use GlifiStore::Client;
my $c = GlifiStore::Client->connect(host => '127.0.0.1', port => 0 + $ENV{GLIFI_PROBE_PORT});
print $c->health(), "\n---\n", $c->ready(), "\n---\n", $c->stats();
$c->close;
PERL
  )"
  health="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==1{gsub(/\n$/,""); print}')"
  ready="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==2{gsub(/\n$/,""); print}')"
  stats="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==3{print}')"
  expect_probes "perl" "$health" "$ready" "$stats"
}

run_ruby() {
  if [[ "$ruby_ready" != "1" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Ruby for probe interop" >&2
      return 1
    fi
    echo "note: Ruby probe interop soft-skipped" >&2
    return 0
  fi
  local out health ready stats
  out="$(
    GLIFI_PROBE_PORT="$port" "$ruby_bin" - <<'RUBY'
require "glifi_store"
cfg = GlifiStore::ClientConfig.defaults
cfg.port = Integer(ENV.fetch("GLIFI_PROBE_PORT"))
c = GlifiStore::Client.connect(cfg)
begin
  print c.health
  print "\n---\n"
  print c.ready
  print "\n---\n"
  print c.stats
ensure
  c.close
end
RUBY
  )"
  health="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==1{gsub(/\n$/,""); print}')"
  ready="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==2{gsub(/\n$/,""); print}')"
  stats="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==3{print}')"
  expect_probes "ruby" "$health" "$ready" "$stats"
}

run_erlang() {
  if [[ "$erlang_ready" != "1" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Erlang/rebar3 for probe interop" >&2
      return 1
    fi
    echo "note: Erlang probe interop soft-skipped" >&2
    return 0
  fi
  local ebin out health ready stats
  if [[ ! -d "$root/sdk/erlang/_build/default/lib/glifistore/ebin" ]]; then
    (cd "$root/sdk/erlang" && rebar3 compile >/dev/null)
  fi
  ebin="$root/sdk/erlang/_build/default/lib/glifistore/ebin"
  out="$(
    GLIFI_PROBE_PORT="$port" \
      erl -noshell -pa "$ebin" -eval '
Port = list_to_integer(os:getenv("GLIFI_PROBE_PORT")),
{ok, C} = glifistore_client:connect(#{host => "127.0.0.1", port => Port}),
{ok, Health} = glifistore_client:health(C),
{ok, Ready} = glifistore_client:ready(C),
{ok, Stats} = glifistore_client:stats(C),
io:format("~s~n---~n~s~n---~n~s", [Health, Ready, Stats]),
ok = glifistore_client:close(C),
halt(0).
'
  )"
  health="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==1{gsub(/\n$/,""); print}')"
  ready="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==2{gsub(/\n$/,""); print}')"
  stats="$(printf '%s\n' "$out" | awk 'BEGIN{RS="---\n"} NR==3{print}')"
  expect_probes "erlang" "$health" "$ready" "$stats"
}

echo "== SDK probe interop against volatile glifistored port=$port =="

run_one() {
  local name="$1"
  shift
  ran=$((ran + 1))
  if ! "$@"; then
    echo "FAIL: $name" >&2
    failures=$((failures + 1))
  fi
}

run_one python run_python
run_one perl run_perl
run_one go run_go
run_one cpp run_cpp
run_one ruby run_ruby
run_one erlang run_erlang

if [[ "$failures" -ne 0 ]]; then
  echo "probe interop failures: $failures / $ran" >&2
  exit 1
fi
echo "probe interop passed ($ran languages)"
