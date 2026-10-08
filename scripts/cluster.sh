#!/usr/bin/env bash
# cluster.sh -- a tracker and a ring of nodes on this machine, with one command (D-064).
#
#   scripts/cluster.sh up [N]     start a tracker and N nodes (default 5); prints how to use them
#   scripts/cluster.sh status     which of the started processes are alive
#   scripts/cluster.sh down       stop exactly the processes `up` started (by PID); keep the data
#   scripts/cluster.sh wipe       down, then delete the cluster directory
#
# Settings, from the environment:
#   CLUSTER_DIR   where stores, logs and PIDs live          (default ./cluster)
#   TRACKER_PORT  the tracker's port                        (default 7000)
#   NODE_BASE     the first node's port; the rest follow    (default 7001)
#   VNODES        ring members per node process (D-051)     (default 8)
#   PERIOD        stabilisation period in ms (D-039)        (default 1000)
#   NODE_ARGS     anything else to pass every node
#
# The first node creates the ring and the others join through it (D-029). `up`
# returns once every process is listening and three stabilisation periods have
# passed, so the ring has settled before the first upload.
#
# `down` kills only the PIDs `up` recorded -- never a machine-wide pkill, which once
# made two test suites kill each other's processes.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${CLUSTER_DIR:-$PWD/cluster}"
TRACKER_PORT="${TRACKER_PORT:-7000}"
NODE_BASE="${NODE_BASE:-7001}"
VNODES="${VNODES:-8}"
PERIOD="${PERIOD:-1000}"
PIDS="$DIR/pids"

listening() { ss -ltn 2>/dev/null | grep -qE "[:.]$1[[:space:]]"; }

wait_listening() {               # wait_listening <port> <pid>: up to 15 s
  local i
  for i in $(seq 150); do
    listening "$1" && return 0
    kill -0 "$2" 2>/dev/null || return 1           # it died: no point waiting
    sleep 0.1
  done
  return 1
}

down() {
  [[ -f "$PIDS" ]] || { echo "cluster: nothing started from $DIR"; return 0; }
  local pid
  while read -r pid _; do kill -9 "$pid" 2>/dev/null; done < "$PIDS"
  while read -r pid _; do
    while kill -0 "$pid" 2>/dev/null; do sleep 0.05; done
  done < "$PIDS"
  rm -f "$PIDS"
  echo "cluster: stopped"
}

up() {
  local n="${1:-5}" i port pid
  [[ "$n" =~ ^[0-9]+$ && "$n" -ge 1 ]] || { echo "cluster: N must be a positive number"; return 2; }
  for b in tracker node client; do
    [[ -x "$ROOT/$b" ]] || { echo "cluster: $ROOT/$b not built; run make"; return 1; }
  done
  [[ -f "$PIDS" ]] && { echo "cluster: already up in $DIR (run down first)"; return 1; }
  for port in "$TRACKER_PORT" $(seq "$NODE_BASE" $((NODE_BASE + n - 1))); do
    listening "$port" && { echo "cluster: port $port is already in use"; return 1; }
  done
  mkdir -p "$DIR/logs"
  : > "$PIDS"

  "$ROOT/tracker" "$TRACKER_PORT" --dir "$DIR" > "$DIR/logs/tracker.log" 2>&1 &
  pid=$!
  echo "$pid tracker" >> "$PIDS"
  wait_listening "$TRACKER_PORT" "$pid" || { echo "cluster: the tracker did not start (see $DIR/logs/tracker.log)"; down; return 1; }

  for i in $(seq 0 $((n - 1))); do
    port=$((NODE_BASE + i))
    if [[ $i -eq 0 ]]; then start=(--create); else start=(--join "127.0.0.1:$NODE_BASE"); fi
    # shellcheck disable=SC2086
    "$ROOT/node" 127.0.0.1 "$port" "${start[@]}" --vnodes "$VNODES" --period "$PERIOD" \
        --store "$DIR/store-$port" ${NODE_ARGS:-} > "$DIR/logs/node-$port.log" 2>&1 &
    pid=$!
    echo "$pid node-$port" >> "$PIDS"
    wait_listening "$port" "$pid" || { echo "cluster: node $port did not start (see $DIR/logs/node-$port.log)"; down; return 1; }
  done
  sleep "$(awk "BEGIN { print 3 * $PERIOD / 1000 }")"       # three rounds: let the ring settle

  local ring="127.0.0.1:$NODE_BASE"
  [[ $n -gt 1 ]] && ring="$ring,127.0.0.1:$((NODE_BASE + 1))"
  echo "cluster: up in $DIR -- tracker 127.0.0.1:$TRACKER_PORT, $n nodes on ports $NODE_BASE-$((NODE_BASE + n - 1)), $VNODES ring members each"
  echo "  $ROOT/client --tracker 127.0.0.1:$TRACKER_PORT --ring $ring put <file> <name>"
  echo "  $ROOT/client --tracker 127.0.0.1:$TRACKER_PORT --ring $ring get <name> <file>"
}

status() {
  [[ -f "$PIDS" ]] || { echo "cluster: not up in $DIR"; return 1; }
  local pid what dead=0
  while read -r pid what; do
    if kill -0 "$pid" 2>/dev/null; then echo "  $what  pid $pid  running"; else echo "  $what  pid $pid  GONE"; dead=1; fi
  done < "$PIDS"
  return $dead
}

case "${1:-}" in
  up)     shift; up "$@" ;;
  down)   down ;;
  status) status ;;
  wipe)   down; rm -rf "$DIR"; echo "cluster: removed $DIR" ;;
  *)      sed -n '2,8p' "$0"; exit 2 ;;
esac
