#!/usr/bin/env bash
# Complete native SDK client benchmark suite against an external glifistored.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date -u +%Y%m%d-%H%M%S)"
sdk_version="$(tr -d '[:space:]' <"$root/VERSION")"
outdir="${1:-$root/benchmark-results-sdk-${sdk_version}-${stamp}}"
daemon="${GLIFISTORED:-$root/build/macos-native-release/glifistored}"
cpp_bench="${CPP_CLIENT_BENCHMARK:-$(dirname "$daemon")/glifistore_client_benchmark}"
python="${PYTHON:-python3}"
perl="${PERL:-perl}"
ruby_bin="${RUBY:-}"
host="127.0.0.1"
ops="${OPS:-100000}"
warmup="${WARMUP:-1}"
repeats="${REPEATS:-7}"
workers_csv="${WORKERS:-1,2,4}"
pipelines_csv="${PIPELINES:-1,8,32,128}"
require_all="${SDK_BENCH_REQUIRE_ALL:-0}"

validate_positive() {
  local label="$1" value="$2"
  if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
    echo "$label must be a positive integer" >&2
    exit 2
  fi
}

validate_csv() {
  local label="$1" value="$2" item
  if [[ ! "$value" =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]]; then
    echo "$label must be a comma-separated list of positive integers" >&2
    exit 2
  fi
  local seen=","
  IFS=',' read -r -a items <<<"$value"
  for item in "${items[@]}"; do
    if [[ "$seen" == *",$item,"* ]]; then
      echo "$label contains duplicate value $item" >&2
      exit 2
    fi
    seen+="$item,"
  done
}

validate_positive OPS "$ops"
validate_positive REPEATS "$repeats"
if [[ ! "$warmup" =~ ^[0-9]+$ ]]; then
  echo "WARMUP must be a non-negative integer" >&2
  exit 2
fi
validate_csv WORKERS "$workers_csv"
validate_csv PIPELINES "$pipelines_csv"
if [[ "$require_all" != "0" && "$require_all" != "1" ]]; then
  echo "SDK_BENCH_REQUIRE_ALL must be 0 or 1" >&2
  exit 2
fi
IFS=',' read -r -a workers <<<"$workers_csv"
IFS=',' read -r -a pipelines <<<"$pipelines_csv"

if [[ ! -x "$daemon" ]]; then
  echo "missing glifistored at $daemon; build macos-native-release first" >&2
  exit 1
fi
if [[ ! -x "$cpp_bench" ]]; then
  echo "missing C++ client benchmark at $cpp_bench; build glifistore_client_benchmark first" >&2
  exit 1
fi

mkdir -p "$outdir/cpp" "$outdir/python" "$outdir/perl" "$outdir/go" "$outdir/erlang" \
  "$outdir/ruby" "$outdir/logs"
export PYTHONPATH="$root/sdk/python/src"
export PERL5LIB="$root/sdk/perl/lib${PERL5LIB:+:$PERL5LIB}"
python_sdk_version="$($python -c 'import glifistore; print(glifistore.__version__)')"
perl_sdk_version="$($perl -MGlifiStore -e 'print $GlifiStore::VERSION')"
if [[ "$python_sdk_version" != "$sdk_version" || "$perl_sdk_version" != "$sdk_version" ]]; then
  echo "source SDK version does not match VERSION (Python=$python_sdk_version Perl=$perl_sdk_version root=$sdk_version)" >&2
  exit 1
fi
go_bin="${GO:-go}"
mkdir -p "$root/sdk/go/bin"
(cd "$root/sdk/go" && "$go_bin" build -o bin/glifistore-bench ./cmd/glifistore-bench)
go_bench="$root/sdk/go/bin/glifistore-bench"
available_sdks="cpp,python,perl,go"

ruby_ready=0
ruby_sdk_version=""
if [[ -z "$ruby_bin" && -x "$HOME/.local/bin/mise" ]]; then
  ruby_bin="$("$HOME/.local/bin/mise" which ruby@3.4 2>/dev/null || true)"
fi
if [[ -z "$ruby_bin" ]]; then
  ruby_bin="$(command -v ruby || true)"
fi
if [[ -n "$ruby_bin" ]] && "$ruby_bin" -rrubygems -e \
  'exit(Gem::Version.new(RUBY_VERSION) >= Gem::Version.new("3.2.0") ? 0 : 1)'
then
  ruby_ready=1
  ruby_sdk_version="$("$ruby_bin" -I"$root/sdk/ruby/lib" \
    -e 'require "glifi_store"; print GlifiStore::VERSION')"
  if [[ "$ruby_sdk_version" != "$sdk_version" ]]; then
    echo "Ruby SDK version '$ruby_sdk_version' does not match '$sdk_version'" >&2
    exit 1
  fi
  "$ruby_bin" -c "$root/sdk/ruby/benchmarks/client_benchmark.rb" >/dev/null
  available_sdks+=",ruby"
else
  echo "note: Ruby SDK bench skipped (set RUBY= to Ruby >= 3.2)" >&2
fi

erlang_ready=0
erlang_bench=""
if command -v erl >/dev/null 2>&1 && command -v rebar3 >/dev/null 2>&1; then
  (cd "$root/sdk/erlang" && rebar3 compile >/dev/null)
  erlang_bench="$root/sdk/erlang/benchmarks/client_benchmark.escript"
  chmod +x "$erlang_bench"
  erlang_ready=1
  erlang_sdk_version="$(erl -noshell -pa "$root/sdk/erlang/_build/default/lib/glifistore/ebin" -eval 'io:format("~s",[glifistore_version:version()]),halt().')"
  if [[ "$erlang_sdk_version" != "$sdk_version" ]]; then
    echo "Erlang SDK version '$erlang_sdk_version' does not match '$sdk_version'" >&2
    exit 1
  fi
  available_sdks+=",erlang"
else
  echo "note: Erlang SDK bench skipped (install OTP + rebar3 to include erlang)" >&2
fi

if [[ "$require_all" == "1" && ( "$ruby_ready" != "1" || "$erlang_ready" != "1" ) ]]; then
  echo "complete SDK comparison requested, but Ruby and/or Erlang is unavailable" >&2
  exit 1
fi

capture_environment() {
  {
    echo "captured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_sha=$(git -C "$root" rev-parse HEAD)"
    echo "git_status=$(git -C "$root" status --porcelain | wc -l | tr -d ' ') dirty paths"
    echo "kernel=$(uname -a)"
    echo "hardware=$(uname -m)"
    if command -v sw_vers >/dev/null 2>&1; then
      echo "product=$(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    fi
    if command -v sysctl >/dev/null 2>&1; then
      echo "cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
      echo "logical_cpu=$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
      echo "physical_cpu=$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
      echo "memory_bytes=$(sysctl -n hw.memsize 2>/dev/null || true)"
    fi
    echo "python=$($python - <<'PY'
import sys
print(sys.version.replace("\n", " "))
PY
)"
    echo "python_sdk_version=$python_sdk_version"
    echo "perl=$($perl -V:version -V:archname | tr '\n' ' ')"
    echo "perl_sdk_version=$perl_sdk_version"
    if [[ "$ruby_ready" == "1" ]]; then
      echo "ruby=$($ruby_bin --version)"
      echo "ruby_sdk_version=$ruby_sdk_version"
    else
      echo "ruby=skipped"
    fi
    echo "go=$($go_bin version)"
    echo "go_sdk_version=$sdk_version"
    echo "cpp_client_benchmark=$cpp_bench"
    if [[ "$erlang_ready" == "1" ]]; then
      echo "erlang=$(erl -noshell -eval 'io:format("~s",[erlang:system_info(otp_release)]),halt().')"
      echo "erlang_sdk_version=$erlang_sdk_version"
    else
      echo "erlang=skipped"
    fi
    echo "glifistored=$daemon"
    echo "glifistored_version=$("$daemon" --version 2>&1 | tr '\n' ' ')"
    echo "ops=$ops warmup=$warmup repeats=$repeats"
    echo "workers=$workers_csv pipelines=$pipelines_csv"
    echo "available_sdks=$available_sdks require_all=$require_all"
    echo "workload=ordered PUT/GET pipeline read-after-write, value_size=64"
    echo "storage_mode=volatile"
    echo "note=same-host loopback; load generator and server share the machine"
  } >"$outdir/environment.txt"
}

start_server() {
  local workers="$1"
  local port_file="$2"
  local log_file="$3"
  "$daemon" \
    --bind "$host" \
    --port 0 \
    --workers "$workers" \
    --storage-mode volatile \
    --executor-affinity \
    --quiet \
    >"$log_file" 2>&1 &
  local pid=$!
  echo "$pid" >"${port_file}.pid"
  # glifistored with --port 0 prints nothing when quiet; discover via lsof.
  local port=""
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "glifistored exited early; see $log_file" >&2
      cat "$log_file" >&2 || true
      return 1
    fi
    port="$(lsof -nP -iTCP -sTCP:LISTEN -a -p "$pid" 2>/dev/null | awk 'NR==2 {split($9,a,":"); print a[length(a)]}')"
    if [[ -n "$port" ]]; then
      echo "$port" >"$port_file"
      return 0
    fi
    sleep 0.1
  done
  echo "could not discover glifistored listen port" >&2
  kill "$pid" 2>/dev/null || true
  return 1
}

stop_server() {
  local port_file="$1"
  if [[ -f "${port_file}.pid" ]]; then
    local pid
    pid="$(cat "${port_file}.pid")"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "${port_file}.pid" "$port_file"
  fi
}

run_matrix() {
  {
    echo "# SDK client benchmark commands"
    echo
    echo "Generated under \`$outdir\`."
    echo
    echo "- SDK version: \`$sdk_version\`"
    echo "- Ops (PUT/GET pairs): \`$ops\`"
    echo "- Warmup/repeats: \`$warmup\` / \`$repeats\`"
    echo "- Workers: \`$workers_csv\`"
    echo "- Pipeline pairs: \`$pipelines_csv\`"
    echo "- Available SDKs: \`$available_sdks\`"
    echo "- Server: volatile \`glifistored\` with \`--executor-affinity\`"
    echo
  } >"$outdir/commands.md"

  for w in "${workers[@]}"; do
    local port_file="$outdir/logs/server-w${w}.port"
    local log_file="$outdir/logs/server-w${w}.log"
    start_server "$w" "$port_file" "$log_file"
    local port
    port="$(cat "$port_file")"
    echo "- workers=$w listen=$host:$port" >>"$outdir/commands.md"

    for p in "${pipelines[@]}"; do
      local label="w${w}-p${p}"
      echo "running C++ concurrent $label"
      "$cpp_bench" --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution concurrent \
        | tee "$outdir/cpp/concurrent-${label}.txt"

      echo "running python sync concurrent $label"
      "$python" "$root/sdk/python/benchmarks/client_benchmark.py" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --runtime sync --execution concurrent \
        | tee "$outdir/python/sync-concurrent-${label}.txt"

      echo "running python sync sequential $label"
      "$python" "$root/sdk/python/benchmarks/client_benchmark.py" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --runtime sync --execution sequential \
        | tee "$outdir/python/sync-sequential-${label}.txt"

      echo "running python async $label"
      "$python" "$root/sdk/python/benchmarks/client_benchmark.py" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --runtime async \
        | tee "$outdir/python/async-concurrent-${label}.txt"

      echo "running perl sequential $label"
      "$perl" "$root/sdk/perl/benchmarks/client_benchmark.pl" \
        --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
        --no-concurrent \
        | tee "$outdir/perl/sequential-${label}.txt"

      if [[ "$w" -gt 1 ]]; then
        echo "running perl concurrent $label"
        "$perl" "$root/sdk/perl/benchmarks/client_benchmark.pl" \
          --host "$host" --port "$port" --workers "$w" --ops "$ops" \
          --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
          --concurrent \
          | tee "$outdir/perl/concurrent-${label}.txt"
      fi

      if [[ "$ruby_ready" == "1" ]]; then
        echo "running ruby sequential $label"
        "$ruby_bin" "$root/sdk/ruby/benchmarks/client_benchmark.rb" \
          --host "$host" --port "$port" --workers "$w" --ops "$ops" \
          --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
          --no-concurrent \
          | tee "$outdir/ruby/sequential-${label}.txt"

        if [[ "$w" -gt 1 ]]; then
          echo "running ruby concurrent $label"
          "$ruby_bin" "$root/sdk/ruby/benchmarks/client_benchmark.rb" \
            --host "$host" --port "$port" --workers "$w" --ops "$ops" \
            --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
            --concurrent \
            | tee "$outdir/ruby/concurrent-${label}.txt"
        fi
      fi

      echo "running go concurrent $label"
      "$go_bench" --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution concurrent \
        | tee "$outdir/go/concurrent-${label}.txt"

      echo "running go sequential $label"
      "$go_bench" --host "$host" --port "$port" --workers "$w" --ops "$ops" \
        --pipeline "$p" --warmup "$warmup" --repeats "$repeats" --execution sequential \
        | tee "$outdir/go/sequential-${label}.txt"

      if [[ "$erlang_ready" == "1" ]]; then
        echo "running erlang sequential $label"
        escript "$erlang_bench" \
          --host "$host" --port "$port" --workers "$w" --ops "$ops" \
          --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
          --no-concurrent \
          | tee "$outdir/erlang/sequential-${label}.txt"

        if [[ "$w" -gt 1 ]]; then
          echo "running erlang concurrent $label"
          escript "$erlang_bench" \
            --host "$host" --port "$port" --workers "$w" --ops "$ops" \
            --pipeline "$p" --warmup "$warmup" --repeats "$repeats" \
            --concurrent \
            | tee "$outdir/erlang/concurrent-${label}.txt"
        fi
      fi
    done

    stop_server "$port_file"
  done
}

write_validated_summary() {
  "$python" "$root/scripts/sdk_benchmark_report.py" \
    --outdir "$outdir" \
    --sdk-version "$sdk_version" \
    --workers "$workers_csv" \
    --pipelines "$pipelines_csv" \
    --ops "$ops" \
    --warmup "$warmup" \
    --repeats "$repeats" \
    --available "$available_sdks"
}

write_readme() {
  cat >"$outdir/README.md" <<EOF
# GlifiStore SDK benchmarks — ${sdk_version}

Generated client-side pipeline benchmarks for the native C++, Python, Perl, Go, Erlang, and Ruby SDKs at version
\`${sdk_version}\`.

## Contents

| Path | Purpose |
| --- | --- |
| \`environment.txt\` | Host, toolchain, SDK, and daemon metadata |
| \`commands.md\` | Workload matrix and listen ports |
| \`summary.md\` | Comparison table (median ops/s) |
| \`results.json\` | Validated matrix, completeness status, and parsed results |
| \`cpp/\` | Raw C++ public-client result files |
| \`python/\` | Raw Python sync/async result files |
| \`perl/\` | Raw Perl result files |
| \`go/\` | Raw Go result files |
| \`erlang/\` | Raw Erlang result files (when OTP/rebar3 available) |
| \`ruby/\` | Raw Ruby result files (when Ruby >= 3.2 available) |
| \`logs/\` | Server stdout/stderr |

## How to reproduce

\`\`\`bash
./scripts/benchmark_sdk_clients.sh
\`\`\`

Language-only:

\`\`\`bash
./scripts/benchmark_go_client.sh
./scripts/benchmark_perl_client.sh
./scripts/benchmark_erlang_client.sh
./scripts/benchmark_ruby_client.sh
\`\`\`

Optional overrides: \`OPS\`, \`WARMUP\`, \`REPEATS\`, \`WORKERS\`, \`PIPELINES\`,
\`SDK_BENCH_REQUIRE_ALL\`, \`GLIFISTORED\`, \`CPP_CLIENT_BENCHMARK\`, \`PYTHON\`,
\`PERL\`, \`GO\`, \`RUBY\`.
EOF
}

cleanup() {
  for pidfile in "$outdir"/logs/*.pid; do
    [[ -f "$pidfile" ]] || continue
    kill "$(cat "$pidfile")" 2>/dev/null || true
  done
}
trap cleanup EXIT

capture_environment
write_readme
run_matrix
write_validated_summary
trap - EXIT
echo "SDK benchmarks published under $outdir"
