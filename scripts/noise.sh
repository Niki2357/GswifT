#!/usr/bin/env bash
# Controlled host noise on this node: CPU load that competes with the network
# stack and the application for cores, to raise the host-delay tail on demand.
#
#   noise.sh up cpu50       stress-ng on every core at 50% duty cycle
#   noise.sh up cpu100      every core busy
#   noise.sh up half100     half the cores busy (the other half left idle)
#   noise.sh down
#   noise.sh show
#
# Run on both nodes for symmetric noise. Needs stress-ng (apt).

set -euo pipefail
LOG=/tmp/gswift_noise.log

down() { pkill -INT -x stress-ng 2>/dev/null || true; sleep 0.5; pkill -KILL -x stress-ng 2>/dev/null || true; }

up() {
    local spec="$1" n load
    command -v stress-ng >/dev/null || { echo "stress-ng not installed" >&2; exit 1; }
    down
    case "$spec" in
        none)      echo "noise: none"; return 0 ;;
        cpu*)      n=0; load="${spec#cpu}" ;;
        half*)     n=$(( $(nproc) / 2 )); load="${spec#half}" ;;
        *) echo "unknown noise '$spec'" >&2; exit 2 ;;
    esac
    [[ "$load" =~ ^[0-9]+$ && "$load" -ge 1 && "$load" -le 100 ]] ||
        { echo "bad load '$load'" >&2; exit 2; }
    nohup stress-ng --cpu "$n" --cpu-load "$load" --cpu-method int64 \
        </dev/null >"$LOG" 2>&1 &
    sleep 1
    pgrep -x stress-ng >/dev/null || { cat "$LOG" >&2; exit 1; }
    echo "noise: $spec ($(pgrep -cx stress-ng) workers)"
}

case "${1:-}" in
    up)   up "${2:?spec}" ;;
    down) down; echo "noise: down" ;;
    show) pgrep -ax stress-ng | head -3 || echo "noise: none" ;;
    *)    echo "usage: $0 up SPEC | down | show" >&2; exit 2 ;;
esac
