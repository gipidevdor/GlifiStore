#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT/engineering/formal/shard_pair"
if [[ -n "${TLA2TOOLS_JAR:-}" && -f "$TLA2TOOLS_JAR" ]]; then JAR="$TLA2TOOLS_JAR"
elif [[ -f /usr/share/java/tla2tools.jar ]]; then JAR=/usr/share/java/tla2tools.jar
else echo "tla2tools.jar not found. Set TLA2TOOLS_JAR." >&2; exit 2; fi
echo "Using $JAR"
exec java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config ShardPair.cfg ShardPair.tla
