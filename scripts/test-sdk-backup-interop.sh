#!/usr/bin/env bash
# Runtime BACKUP interop: durable glifistored + typed backup() in each official SDK.
# Clears the residual left by scripts/assert-sdk-backup-helpers.sh (symbol-only).
# Soft-skips languages whose toolchain is absent unless BACKUP_INTEROP_REQUIRE_ALL=1.
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

to_hex() {
  if command -v xxd >/dev/null 2>&1; then
    xxd -p -c 256 | tr -d '\n'
  else
    od -An -tx1 | tr -d ' \n'
  fi
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

work="$(mktemp -d "${TMPDIR:-/tmp}/glifistore-backup-interop.XXXXXX")"
daemon_pid=""
cleanup() {
  if [[ -n "${daemon_pid:-}" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

# create-new requires a path that does not exist yet (daemon creates it).
data_dir="$work/store"
log_out="$work/daemon.out"
log_err="$work/daemon.err"

"$daemon" --quiet --bind 127.0.0.1 --port 0 --shard-pairs 1 \
  --storage-mode durable-sync --data-dir "$data_dir" --open-mode create-new \
  --shutdown-drain-ms 2000 \
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

require_all="${BACKUP_INTEROP_REQUIRE_ALL:-0}"
failures=0
ran=0

expect_ok_report() {
  local label="$1"
  local report="$2"
  if [[ "$report" != *status=ok* ]]; then
    echo "FAIL $label: expected status=ok in report, got: $report" >&2
    return 1
  fi
  echo "ok: $label backup report contains status=ok"
}

run_cpp() {
  if [[ -z "$cpp_client" || ! -x "$cpp_client" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing glifistore_interop_client for C++ backup interop" >&2
      return 1
    fi
    echo "note: C++ backup interop soft-skipped (no interop client)" >&2
    return 0
  fi
  local dest="$work/backup-cpp"
  local key_hex value_hex report
  key_hex="$(printf 'sdk-backup-cpp' | to_hex)"
  value_hex="$(printf 'cpp-value' | to_hex)"
  "$cpp_client" --host 127.0.0.1 --port "$port" --key-hex "$key_hex" --value-hex "$value_hex" put
  report="$("$cpp_client" --host 127.0.0.1 --port "$port" --dest "$dest" backup)"
  expect_ok_report "cpp" "$report"
}

run_python() {
  local dest="$work/backup-python"
  local report
  report="$("$python" - <<PY
from glifistore.client import Client, ClientConfig
c = Client.connect(ClientConfig(host="127.0.0.1", port=$port))
try:
    put = c.put(b"sdk-backup-py", b"py-value")
    assert put.committed, put
    report = c.backup("$dest")
    print(report.decode("utf-8", errors="replace"))
finally:
    c.close()
PY
)"
  expect_ok_report "python" "$report"
}

run_go() {
  if [[ -z "$go_helper" || ! -x "$go_helper" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Go interop helper" >&2
      return 1
    fi
    echo "note: Go backup interop soft-skipped (no go helper)" >&2
    return 0
  fi
  local dest="$work/backup-go"
  local key_hex value_hex report
  key_hex="$(printf 'sdk-backup-go' | to_hex)"
  value_hex="$(printf 'go-value' | to_hex)"
  "$go_helper" --host 127.0.0.1 --port "$port" --key-hex "$key_hex" --value-hex "$value_hex" put
  report="$("$go_helper" --host 127.0.0.1 --port "$port" --dest "$dest" backup)"
  expect_ok_report "go" "$report"
}

run_perl() {
  local dest="$work/backup-perl"
  local report
  report="$(
    GLIFI_BACKUP_PORT="$port" GLIFI_BACKUP_DEST="$dest" "$perl" - <<'PERL'
use strict;
use warnings;
use GlifiStore::Client;
my $port = $ENV{GLIFI_BACKUP_PORT};
my $dest = $ENV{GLIFI_BACKUP_DEST};
my $c = GlifiStore::Client->connect(host => '127.0.0.1', port => 0 + $port);
my $put = $c->put('sdk-backup-pl', 'pl-value');
die 'put failed' unless $put && ($put->{outcome} // '') eq 'committed';
print $c->backup($dest);
$c->close;
PERL
  )"
  expect_ok_report "perl" "$report"
}

run_ruby() {
  if [[ "$ruby_ready" != "1" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Ruby for backup interop" >&2
      return 1
    fi
    echo "note: Ruby backup interop soft-skipped" >&2
    return 0
  fi
  local dest="$work/backup-ruby"
  local report
  report="$(
    GLIFI_BACKUP_PORT="$port" GLIFI_BACKUP_DEST="$dest" "$ruby_bin" - <<'RUBY'
require "glifi_store"
port = Integer(ENV.fetch("GLIFI_BACKUP_PORT"))
dest = ENV.fetch("GLIFI_BACKUP_DEST")
cfg = GlifiStore::ClientConfig.defaults
cfg.port = port
c = GlifiStore::Client.connect(cfg)
begin
  r = c.put("sdk-backup-rb", "rb-value")
  raise "put failed" unless r.committed?
  print c.backup(dest)
ensure
  c.close
end
RUBY
  )"
  expect_ok_report "ruby" "$report"
}

run_erlang() {
  if [[ "$erlang_ready" != "1" ]]; then
    if [[ "$require_all" == "1" ]]; then
      echo "missing Erlang/rebar3 for backup interop" >&2
      return 1
    fi
    echo "note: Erlang backup interop soft-skipped" >&2
    return 0
  fi
  local dest="$work/backup-erlang"
  local ebin report
  if [[ ! -d "$root/sdk/erlang/_build/default/lib/glifistore/ebin" ]]; then
    (cd "$root/sdk/erlang" && rebar3 compile >/dev/null)
  fi
  ebin="$root/sdk/erlang/_build/default/lib/glifistore/ebin"
  report="$(
    GLIFI_BACKUP_PORT="$port" GLIFI_BACKUP_DEST="$dest" \
      erl -noshell -pa "$ebin" -eval '
Port = list_to_integer(os:getenv("GLIFI_BACKUP_PORT")),
Dest = list_to_binary(os:getenv("GLIFI_BACKUP_DEST")),
{ok, C} = glifistore_client:connect(#{host => "127.0.0.1", port => Port}),
#{outcome := committed} = glifistore_client:put(C, <<"sdk-backup-erl">>, <<"erl-value">>),
{ok, Report} = glifistore_client:backup(C, Dest),
io:put_chars(Report),
ok = glifistore_client:close(C),
halt(0).
'
  )"
  expect_ok_report "erlang" "$report"
}

echo "== SDK backup interop against durable glifistored port=$port =="

run_one() {
  local name="$1"
  shift
  ran=$((ran + 1))
  if ! "$@"; then
    echo "FAIL: $name" >&2
    failures=$((failures + 1))
  fi
}

run_one cpp run_cpp
run_one python run_python
run_one perl run_perl
run_one go run_go
run_one ruby run_ruby
run_one erlang run_erlang

if [[ "$failures" -ne 0 ]]; then
  echo "test-sdk-backup-interop: $failures failure(s) of $ran language check(s)" >&2
  exit 1
fi
echo "test-sdk-backup-interop: $ran language check(s) ok (fenced online BACKUP; not zero-fence)"
