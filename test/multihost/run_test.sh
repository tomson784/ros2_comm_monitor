#!/bin/bash
# Multi-host test with docker compose. Exit code 0 = all checks passed.
#
#   test/multihost/run_test.sh [OUT_DIR]
#
# Phases:
#   1. auto : every PC runs its agent with locality_mode=auto
#   2. all  : master agent restarted with locality_mode=all (pulls /big from host-b)
#   3. stop host_b -> it must turn offline after agent_timeout_sec (5 s)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-${HERE}/results}"
SETTLE_SEC="${SETTLE_SEC:-15}"
MEASURE_SEC="${MEASURE_SEC:-10}"
mkdir -p "${OUT}"
rm -f "${OUT}"/*.json "${OUT}"/*.txt "${OUT}"/*.log
dc() { docker compose -f "${HERE}/docker-compose.yml" "$@"; }

cleanup() { dc logs --no-color > "${OUT}/compose.log" 2>&1 || true; dc down -t 5 > /dev/null 2>&1 || true; }
trap cleanup EXIT

api() { dc exec -T master curl -sf "http://localhost:8080/api/$1"; }
snap() { for a in stats graph hosts; do api "$a" > "${OUT}/$1_${a}.json"; done; }
rx_bytes() { dc exec -T master cat /sys/class/net/eth0/statistics/rx_bytes | tr -d '\r'; }
measure() {
  local r0 r1; r0=$(rx_bytes); sleep "${MEASURE_SEC}"; r1=$(rx_bytes)
  echo "$1 $(( (r1 - r0) / MEASURE_SEC ))" >> "${OUT}/rx.txt"
  echo "  $1: master eth0 rx = $(( (r1 - r0) / MEASURE_SEC )) B/s"
}

echo "== build images"
dc build > "${OUT}/build.log" 2>&1 || { tail -30 "${OUT}/build.log"; exit 1; }

echo "== phase 1: locality_mode=auto"
MASTER_LOCALITY_MODE=auto dc up -d > /dev/null
sleep "${SETTLE_SEC}"
measure auto
snap auto

echo "== phase 2: master locality_mode=all"
MASTER_LOCALITY_MODE=all dc up -d --force-recreate --no-deps master > /dev/null
sleep "${SETTLE_SEC}"
measure all
snap all

echo "== phase 3: stop host_b"
dc stop -t 5 host_b > /dev/null
date +%s.%N > "${OUT}/host_b_stopped.txt"
sleep 8
snap offline

for s in master host_b host_c; do dc logs --no-color "$s" 2>&1 | grep -E "Agent host=|ROLE=" | sed 's/^.*\]: //' ; done > "${OUT}/agents.txt" || true

echo "== checks"
python3 "${HERE}/check.py" "${OUT}"
