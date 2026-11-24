#!/bin/bash
# Bring up the 3-PC docker setup with a few extra demo topics and capture UI
# screenshots into docs/screenshots/ (requires python3 playwright + chromium).
#
# Extra demo topics (not part of run_test.sh):
#   /shared_status : published on host-b AND host-c -> "mixed", NOT MEASURED
#   /missing_topic : only subscribed on host-c      -> "orphan", DOWN
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-${HERE}/../../docs/screenshots}"
PORT="${MASTER_HTTP_PORT:-18090}"
dc() { docker compose -f "${HERE}/docker-compose.yml" "$@"; }
trap 'dc down -t 5 > /dev/null 2>&1 || true' EXIT

dc build > /dev/null
MASTER_LOCALITY_MODE=auto dc up -d > /dev/null
in_host() { local svc=$1; shift; dc exec -d "$svc" /tools/entrypoint.sh "$@"; }
in_host host_b ros2 topic pub -r 1 /shared_status std_msgs/msg/String "{data: b}"
in_host host_c ros2 topic pub -r 1 /shared_status std_msgs/msg/String "{data: c}"
in_host host_c ros2 topic echo /missing_topic std_msgs/msg/String
sleep 15

echo "== online"
python3 "${HERE}/capture_ui.py" "http://127.0.0.1:${PORT}/" "${OUT}" online
mkdir -p "${HERE}/results"; curl -s "http://127.0.0.1:${PORT}/api/stats" > "${HERE}/results/demo_stats.json" || true

echo "== host_b stopped"
dc stop -t 5 host_b > /dev/null
sleep 7
python3 "${HERE}/capture_ui.py" "http://127.0.0.1:${PORT}/" "${OUT}" offline
