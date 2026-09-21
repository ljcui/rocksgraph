#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
server_bin=${ROCKSGRAPH_TCK_SERVER_BIN:-"$repo_root/build/server/rg-server"}
server_pid=
server_tmp=

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_tmp" ]]; then
    rm -rf "$server_tmp"
  fi
}
trap cleanup EXIT INT TERM

python3 -c 'import behave, neo4j' >/dev/null

if [[ -z "${ROCKSGRAPH_TCK_URI:-}" ]]; then
  if [[ ! -x "$server_bin" ]]; then
    echo "rg-server not found: $server_bin" >&2
    echo "Build it first or set ROCKSGRAPH_TCK_SERVER_BIN/ROCKSGRAPH_TCK_URI." >&2
    exit 2
  fi
  bolt_port=${ROCKSGRAPH_TCK_BOLT_PORT:-7687}
  raft_port=${ROCKSGRAPH_TCK_RAFT_PORT:-$((bolt_port + 1))}
  server_tmp=$(mktemp -d /tmp/rocksgraph_tck_server.XXXXXX)
  "$server_bin" --data_path="$server_tmp/data" \
    --log_path="$server_tmp/log" --query_log_path="$server_tmp/log" \
    --log_level=error --host=127.0.0.1 --bolt_port="$bolt_port" \
    --raft_port="$raft_port" >"$server_tmp/server.log" 2>&1 &
  server_pid=$!
  export ROCKSGRAPH_TCK_URI="bolt://127.0.0.1:$bolt_port"
  ready=
  for _ in $(seq 1 100); do
    if python3 - <<'PY' 2>/dev/null
import os
from neo4j import GraphDatabase

driver = GraphDatabase.driver(
    os.environ["ROCKSGRAPH_TCK_URI"],
    auth=(
        os.environ.get("ROCKSGRAPH_TCK_USER", "neo4j"),
        os.environ.get("ROCKSGRAPH_TCK_PASSWORD", "password"),
    ),
)
driver.verify_connectivity()
driver.close()
PY
    then
      ready=1
      break
    fi
    sleep 0.1
  done
  if [[ -z "$ready" ]]; then
    echo "rg-server did not become ready: $server_tmp/server.log" >&2
    cat "$server_tmp/server.log" >&2
    exit 1
  fi
fi

cd "$script_dir"
exec behave "$@"
