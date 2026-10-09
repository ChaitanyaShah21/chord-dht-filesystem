#!/usr/bin/env bash
# cluster.sh -- a tracker group and a ring of nodes on this machine, with one command (D-064).
#
#   scripts/cluster.sh up [N]     start the tracker replicas and N nodes (default 5); prints how to use them
#   scripts/cluster.sh status     which of the started processes are alive
#   scripts/cluster.sh down       stop exactly the processes `up` started (by PID); keep the data
#   scripts/cluster.sh wipe       down, then delete the cluster directory
#
# Settings, from the environment:
#   CLUSTER_DIR   where stores, logs and PIDs live          (default ./cluster)
#   TRACKERS      tracker replicas, a Raft group (D-069)    (default 3; 1 is a single tracker)
#   TRACKER_PORT  the first replica's port; the rest follow (default 7000)
#   NODE_BASE     the first node's port; the rest follow    (default 7010)
#   VNODES        ring members per node process (D-051)     (default 8)
#   PERIOD        stabilisation period in ms (D-039)        (default 1000)
#   NODE_ARGS     anything else to pass every node
#
# The first node creates the ring and the others join through it (D-029). `up`
# returns once every process is listening, the tracker group answers (it has
# elected a leader), and three stabilisation periods have passed, so the ring has
# settled before the first upload.
#
# `down` kills only the PIDs `up` recorded -- never a machine-wide pkill, which once
# made two test suites kill each other's processes.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${CLUSTER_DIR:-$PWD/cluster}"
TRACKERS="${TRACKERS:-3}"
TRACKER_PORT="${TRACKER_PORT:-7000}"
NODE_BASE="${NODE_BASE:-7010}"
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
  [[ "$TRACKERS" =~ ^[0-9]+$ && "$TRACKERS" -ge 1 ]] || { echo "cluster: TRACKERS must be a positive number"; return 2; }
  local tports group
  tports=$(seq "$TRACKER_PORT" $((TRACKER_PORT + TRACKERS - 1)))
  group=$(for port in $tports; do printf '127.0.0.1:%s,' "$port"; done)
  group="${group%,}"
  for b in tracker node client; do
    [[ -x "$ROOT/$b" ]] || { echo "cluster: $ROOT/$b not built; run make"; return 1; }
  done
  [[ -f "$PIDS" ]] && { echo "cluster: already up in $DIR (run down first)"; return 1; }
  for port in $tports $(seq "$NODE_BASE" $((NODE_BASE + n - 1))); do
    listening "$port" && { echo "cluster: port $port is already in use"; return 1; }
  done
  mkdir -p "$DIR/logs"
  : > "$PIDS"

  # The tracker replicas: one group, every replica given the same list (D-079).
  i=0
  for port in $tports; do
    if [[ $TRACKERS -eq 1 ]]; then tgroup=(); else tgroup=(--group "$group" --id "$i"); fi
    mkdir -p "$DIR/tracker-$port"
    "$ROOT/tracker" "$port" --dir "$DIR/tracker-$port" --ip 127.0.0.1 "${tgroup[@]}" > "$DIR/logs/tracker-$port.log" 2>&1 &
    pid=$!
    echo "$pid tracker-$port" >> "$PIDS"
    wait_listening "$port" "$pid" || { echo "cluster: tracker $port did not start (see $DIR/logs/tracker-$port.log)"; down; return 1; }
    i=$((i + 1))
  done
  # Answered by the group only once it has a leader that has committed in its term.
  "$ROOT/client" --tracker "$group" --tracker-wait 15000 ls > /dev/null 2>&1 ||
    { echo "cluster: the tracker group elected no leader (see $DIR/logs/tracker-*.log)"; down; return 1; }

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
  echo "cluster: up in $DIR -- $TRACKERS tracker replica(s) $group, $n nodes on ports $NODE_BASE-$((NODE_BASE + n - 1)), $VNODES ring members each"
  echo "  $ROOT/client --tracker $group --ring $ring put <file> <name>"
  echo "  $ROOT/client --tracker $group --ring $ring get <name> <file>"
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
