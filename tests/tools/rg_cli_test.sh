#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <rg-server> <rg-cli>" >&2
  exit 2
fi

server_bin=$1
cli_bin=$2
test_dir=$(mktemp -d /tmp/rocksgraph_rg_cli_test.XXXXXX)
server_pid=

cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
      if ! kill -0 "$server_pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$server_pid" 2>/dev/null; then
      kill -KILL "$server_pid" 2>/dev/null || true
    fi
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ $status -ne 0 ]]; then
    echo "rg-server output:" >&2
    sed -n '1,200p' "$test_dir/server.out" >&2 || true
  fi
  rm -rf "$test_dir"
  exit "$status"
}
trap cleanup EXIT

read -r bolt_port raft_port < <(
  python3 - <<'PY'
import socket

sockets = []
for _ in range(2):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sockets.append(sock)
print(*(sock.getsockname()[1] for sock in sockets))
PY
)

"$server_bin" \
  --mode=run \
  --data_path="$test_dir/data" \
  --host=127.0.0.1 \
  --bolt_port="$bolt_port" \
  --raft_port="$raft_port" \
  --log_path="$test_dir/log" \
  --log_level=error >"$test_dir/server.out" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 50); do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "rg-server exited before accepting connections" >&2
    exit 1
  fi
  if printf 'RETURN 0;\n' | "$cli_bin" --ip=127.0.0.1 \
      --port="$bolt_port" --format=json >/dev/null 2>&1; then
    server_ready=true
    break
  fi
  sleep 0.1
done
if [[ $server_ready != true ]]; then
  echo "rg-server did not become ready" >&2
  exit 1
fi

json_output=$(
  printf "RETURN\n  1 AS n, 'hello' AS text;\n" |
    "$cli_bin" --ip=127.0.0.1 --port="$bolt_port" --format=json
)
expected_json=$'["n","text"]\n[1,"hello"]'
if [[ "$json_output" != "$expected_json" ]]; then
  echo "unexpected JSON output:" >&2
  printf '%s\n' "$json_output" >&2
  exit 1
fi

csv_output=$(
  printf "RETURN 'a,b' AS value;\n" |
    "$cli_bin" --ip=127.0.0.1 --port="$bolt_port" --format=csv
)
expected_csv=$'value\n"a,b"'
if [[ "$csv_output" != "$expected_csv" ]]; then
  echo "unexpected CSV output:" >&2
  printf '%s\n' "$csv_output" >&2
  exit 1
fi

table_output=$(
  printf 'RETURN 42 AS answer;\n' |
    "$cli_bin" --ip=127.0.0.1 --port="$bolt_port" --format=table
)
if [[ "$table_output" != *answer* ]] || [[ "$table_output" != *42* ]] ||
  [[ "$table_output" != *"1 rows ("* ]]; then
  echo "unexpected table output:" >&2
  printf '%s\n' "$table_output" >&2
  exit 1
fi

recovery_output=$(
  printf 'THIS IS NOT CYPHER;\nRETURN 3 AS n;\n' |
    "$cli_bin" --ip=127.0.0.1 --port="$bolt_port" --format=json \
      2>"$test_dir/recovery.err"
)
expected_recovery=$'["n"]\n[3]'
if [[ "$recovery_output" != "$expected_recovery" ]]; then
  echo "rg-cli did not recover after a failed query:" >&2
  printf '%s\n' "$recovery_output" >&2
  exit 1
fi
if [[ ! -s "$test_dir/recovery.err" ]]; then
  echo "rg-cli did not report the failed query" >&2
  exit 1
fi
