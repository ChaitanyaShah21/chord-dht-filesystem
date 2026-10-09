#!/usr/bin/env bash
# observe.sh -- Prometheus and Grafana against the local cluster (D-088).
#
#   scripts/observe.sh up      start both; Grafana on http://127.0.0.1:3000/d/chordfs
#   scripts/observe.sh down    stop exactly the processes `up` started (by PID)
#   scripts/observe.sh status
#
# Needs a cluster from scripts/cluster.sh (it writes the list of metrics addresses,
# $CLUSTER_DIR/targets.json, that Prometheus reads), and the official release
# archives of Prometheus and Grafana unpacked somewhere outside the repository:
#
#   PROMETHEUS_HOME  the directory holding the `prometheus` binary
#                    (default: the newest ~/.local/opt/prometheus-*)
#   GRAFANA_HOME     the directory holding bin/grafana
#                    (default: the newest ~/.local/opt/grafana-*)
#   OBSERVE_DIR      where their data, logs and PIDs go  (default $CLUSTER_DIR/observe)
#   PROM_PORT        default 9090;  GRAFANA_PORT  default 3000
#
# Neither is linked into anything: they are tools, like Python for the harnesses.
# Downloads (arm64 shown; amd64 is the same with linux-amd64):
#   https://github.com/prometheus/prometheus/releases  prometheus-<v>.linux-arm64.tar.gz
#   https://grafana.com/grafana/download                grafana-<v>.linux-arm64.tar.gz
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLUSTER_DIR="${CLUSTER_DIR:-$PWD/cluster}"
DIR="${OBSERVE_DIR:-$CLUSTER_DIR/observe}"
PROM_PORT="${PROM_PORT:-9090}"
GRAFANA_PORT="${GRAFANA_PORT:-3000}"
PIDS="$DIR/pids"

newest() { ls -d $1 2>/dev/null | sort -V | tail -1; }
PROMETHEUS_HOME="${PROMETHEUS_HOME:-$(newest "$HOME/.local/opt/prometheus-*")}"
GRAFANA_HOME="${GRAFANA_HOME:-$(newest "$HOME/.local/opt/grafana-*")}"

listening() { ss -ltn 2>/dev/null | grep -qE "[:.]$1[[:space:]]"; }

wait_http() {                    # wait_http <url> <pid>: up to 60 s for a 200
  local i
  for i in $(seq 600); do
    curl -sf -o /dev/null "$1" && return 0
    kill -0 "$2" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

down() {
  [[ -f "$PIDS" ]] || { echo "observe: nothing started from $DIR"; return 0; }
  local pid
  while read -r pid _; do kill "$pid" 2>/dev/null; done < "$PIDS"
  while read -r pid _; do
    for _ in $(seq 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$pid" 2>/dev/null
  done < "$PIDS"
  rm -f "$PIDS"
  echo "observe: stopped"
}

up() {
  [[ -x "$PROMETHEUS_HOME/prometheus" ]] || { echo "observe: no prometheus binary (set PROMETHEUS_HOME)"; return 1; }
  [[ -x "$GRAFANA_HOME/bin/grafana" ]] || { echo "observe: no grafana binary (set GRAFANA_HOME)"; return 1; }
  [[ -f "$CLUSTER_DIR/targets.json" ]] || { echo "observe: no $CLUSTER_DIR/targets.json -- start scripts/cluster.sh first"; return 1; }
  [[ -f "$PIDS" ]] && { echo "observe: already up in $DIR (run down first)"; return 1; }
  for port in "$PROM_PORT" "$GRAFANA_PORT"; do
    listening "$port" && { echo "observe: port $port is already in use"; return 1; }
  done
  mkdir -p "$DIR/prometheus-data" "$DIR/grafana-data" "$DIR/grafana-logs" "$DIR/grafana-plugins"
  : > "$PIDS"

  # Prometheus reads targets.json from beside its configuration, and re-reads it
  # whenever it changes; a symbolic link keeps it the cluster's own file.
  cp "$ROOT/deploy/prometheus.yml" "$DIR/prometheus.yml"
  ln -sf "$CLUSTER_DIR/targets.json" "$DIR/targets.json"
  "$PROMETHEUS_HOME/prometheus" --config.file="$DIR/prometheus.yml" --storage.tsdb.path="$DIR/prometheus-data" \
      --web.listen-address="127.0.0.1:$PROM_PORT" --storage.tsdb.retention.time=2d \
      > "$DIR/prometheus.log" 2>&1 &
  echo "$! prometheus" >> "$PIDS"
  wait_http "http://127.0.0.1:$PROM_PORT/-/ready" "$!" || { echo "observe: prometheus did not start (see $DIR/prometheus.log)"; down; return 1; }

  # Grafana, configured entirely from the environment: anonymous viewers, nothing
  # sent home, and the data source and dashboard provisioned from deploy/.
  GF_SERVER_HTTP_ADDR=127.0.0.1 GF_SERVER_HTTP_PORT="$GRAFANA_PORT" \
  GF_PATHS_DATA="$DIR/grafana-data" GF_PATHS_LOGS="$DIR/grafana-logs" GF_PATHS_PLUGINS="$DIR/grafana-plugins" \
  GF_PATHS_PROVISIONING="$ROOT/deploy/grafana/provisioning" \
  PROM_URL="http://127.0.0.1:$PROM_PORT" DASHBOARDS_DIR="$ROOT/deploy/grafana/dashboards" \
  GF_AUTH_ANONYMOUS_ENABLED=true GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer GF_AUTH_DISABLE_LOGIN_FORM=false \
  GF_ANALYTICS_REPORTING_ENABLED=false GF_ANALYTICS_CHECK_FOR_UPDATES=false GF_ANALYTICS_CHECK_FOR_PLUGIN_UPDATES=false \
  GF_NEWS_NEWS_FEED_ENABLED=false GF_DASHBOARDS_DEFAULT_HOME_DASHBOARD_PATH="$ROOT/deploy/grafana/dashboards/chordfs.json" \
  "$GRAFANA_HOME/bin/grafana" server --homepath "$GRAFANA_HOME" > "$DIR/grafana.log" 2>&1 &
  echo "$! grafana" >> "$PIDS"
  wait_http "http://127.0.0.1:$GRAFANA_PORT/api/health" "$!" || { echo "observe: grafana did not start (see $DIR/grafana.log)"; down; return 1; }

  echo "observe: up -- Prometheus http://127.0.0.1:$PROM_PORT  Grafana http://127.0.0.1:$GRAFANA_PORT/d/chordfs"
}

status() {
  [[ -f "$PIDS" ]] || { echo "observe: not up in $DIR"; return 1; }
  local pid what dead=0
  while read -r pid what; do
    if kill -0 "$pid" 2>/dev/null; then echo "  $what  pid $pid  running"; else echo "  $what  pid $pid  GONE"; dead=1; fi
  done < "$PIDS"
  return $dead
}

case "${1:-}" in
  up)     up ;;
  down)   down ;;
  status) status ;;
  *)      sed -n '2,6p' "$0"; exit 2 ;;
esac
