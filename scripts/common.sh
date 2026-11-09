#!/usr/bin/env bash
# Shared settings and helpers for experiment scripts. Source, do not run.
#
# Topology (2 nodes): node0 = sender (runs these scripts, Swift loaded here),
# node1 = receiver (bottleneck on its ingress). Override with env vars:
#   PEER=node1  PEER_IP=10.10.1.2  SELF_IP=10.10.1.1  OUT_ROOT=/mydata/gswift

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SWIFTCTL="$REPO/src/swiftctl"
WORKER="$REPO/tools/incast/incast_worker"
AGG="$REPO/tools/incast/incast_agg"

# Experiment interface: the one holding a 10.10.1.x address.
IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}"
SELF_IP="${SELF_IP:-$(ip -br addr show "$IFACE" 2>/dev/null | awk '{split($3,a,"/"); print a[1]}')}"
PEER="${PEER:-node1}"
PEER_IP="${PEER_IP:-10.10.1.2}"
if [[ -z "${OUT_ROOT:-}" ]]; then
    if [[ -d /mydata && -w /mydata ]]; then OUT_ROOT=/mydata/gswift; else OUT_ROOT="$HOME/gswift_results"; fi
fi
DATE="$(date +%Y-%m-%d)"

# Bottleneck defaults (receiver ingress).
BN_RATE="${BN_RATE:-10gbit}"
BN_LIMIT="${BN_LIMIT:-300}"          # packets
BN_ECN_US="${BN_ECN_US:-50}"         # DCTCP marking threshold (sojourn time)

IPERF_PORT=5201
PROBE_PORT=6001
INCAST_PORT=6000

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

peer() { ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$PEER" "$@"; }

check_env() {
    allow_all_cc
    [[ -n "$IFACE" ]] || die "no interface with a 10.10.1.x address"
    [[ -x "$SWIFTCTL" ]] || die "swiftctl not built; run scripts/setup_node.sh"
    [[ -x "$WORKER" && -x "$AGG" ]] || die "incast tools not built"
    peer true 2>/dev/null || die "cannot ssh to $PEER without a password.
  Log in to node0 with agent forwarding (ssh -A ...) and retry."
    peer test -x "$AGG" || die "repo not built on $PEER at $REPO"
    [[ "$(sysctl -n net.ipv4.tcp_slow_start_after_idle)" == 0 ]] ||
        log "WARNING: session settings missing; run scripts/tune_session.sh on both nodes"
}

# New run directory: $OUT_ROOT/$DATE/<exp>/<name>
run_dir() {
    local d="$OUT_ROOT/$DATE/$1/$2"
    mkdir -p "$d"
    echo "$d"
}

# Non-root programs (iperf3, incast) may only select algorithms listed in
# tcp_allowed_congestion_control, which defaults to "reno cubic". Allow every
# registered one; re-run after registering swift.
ALLOW_CC='sudo sysctl -qw net.ipv4.tcp_allowed_congestion_control="$(cat /proc/sys/net/ipv4/tcp_available_congestion_control)"'
allow_all_cc() { eval "$ALLOW_CC"; }

# Swift loader lifecycle. Extra args go to swiftctl.
SWIFT_PID=""
start_swift() {
    local dir="$1"; shift
    # Clear any leftover instance so this run's parameters are the ones live.
    sudo pkill -INT -x swiftctl 2>/dev/null || true
    for _ in $(seq 30); do
        grep -qw swift /proc/sys/net/ipv4/tcp_available_congestion_control || break
        sleep 0.1
    done
    sudo "$SWIFTCTL" --trace "$dir/trace.csv" --stats "$dir/swift_stats.txt" "$@" \
        </dev/null >/dev/null 2>"$dir/swiftctl.log" &
    SWIFT_PID=$!
    for _ in $(seq 50); do
        grep -qw swift /proc/sys/net/ipv4/tcp_available_congestion_control && { allow_all_cc; return 0; }
        # The sudo wrapper is root-owned, so probe it with sudo.
        sudo kill -0 "$SWIFT_PID" 2>/dev/null || { cat "$dir/swiftctl.log" >&2; die "swiftctl exited"; }
        sleep 0.1
    done
    die "swift did not register; see $dir/swiftctl.log"
}

stop_swift() {
    [[ -n "$SWIFT_PID" ]] || return 0
    # Signal swiftctl itself: sudo ignores signals from its own process group.
    sudo pkill -INT -x swiftctl 2>/dev/null || true
    wait "$SWIFT_PID" 2>/dev/null || true
    SWIFT_PID=""
}

# Bottleneck on the receiver.
bn_up()   { peer sudo "$REPO/scripts/bottleneck.sh" up "${1:-$BN_RATE}" "${2:-$BN_LIMIT}" "${3:-$BN_ECN_US}"; }
bn_down() { peer sudo "$REPO/scripts/bottleneck.sh" down || true; }

# Queue monitor on the receiver, writing to a peer-local temp file.
QMON_REMOTE=/tmp/gswift_qmon.csv
qmon_start() { peer "nohup sudo python3 $REPO/scripts/qmon.py ifb0 $QMON_REMOTE </dev/null >/dev/null 2>&1 & echo \$! > /tmp/gswift_qmon.pid"; }
qmon_stop()  {
    local dir="$1"
    peer 'sudo kill $(cat /tmp/gswift_qmon.pid) 2>/dev/null; sleep 0.3' || true
    scp -q -o BatchMode=yes "$PEER:$QMON_REMOTE" "$dir/qmon.csv" 2>/dev/null || log "no qmon data"
    peer sudo tc -s qdisc show dev ifb0 > "$dir/tc_final.txt" 2>&1 || true
}

# Latency probe: 64 B request/response, response crosses the bottleneck.
PROBE_WORKER_PID=""
probe_server_start() {
    "$WORKER" -p "$PROBE_PORT" -C cubic </dev/null >/dev/null 2>&1 &
    PROBE_WORKER_PID=$!
}
probe_server_stop() { [[ -n "$PROBE_WORKER_PID" ]] && kill "$PROBE_WORKER_PID" 2>/dev/null || true; PROBE_WORKER_PID=""; }
probe_run() {  # probe_run <dir> <seconds>   (runs in background on peer)
    peer "$AGG -s $SELF_IP:$PROBE_PORT -n 1 -b 64 -t $2 -g 200 -w 50 -o /tmp/gswift_probe.csv" \
        2>"$1/probe.log" &
    PROBE_PID=$!
}
probe_collect() {
    wait "$PROBE_PID" 2>/dev/null || log "probe exited non-zero (see probe.log)"
    scp -q -o BatchMode=yes "$PEER:/tmp/gswift_probe.csv" "$1/probe.csv" 2>/dev/null || log "no probe data"
}

nstat_reset() { nstat -n >/dev/null 2>&1 || true; }
nstat_save()  { nstat -z > "$1/nstat.txt" 2>&1 || true; }  # deltas since nstat_reset

write_meta() {  # write_meta <dir> key=value ...
    local dir="$1"; shift
    {
        echo "date=$(date -Is)"
        echo "host=$(hostname)"
        echo "iface=$IFACE"
        echo "bn_rate=$BN_RATE"
        echo "bn_limit=$BN_LIMIT"
        echo "bn_ecn_us=$BN_ECN_US"
        printf '%s\n' "$@"
    } > "$dir/meta.txt"
}

# iperf3's server applies the client's -C algorithm to its own sockets and
# fails if it is not registered there, so the receiver also loads Swift. A
# pure receiver sends no data, so this does not affect results.
peer_swift_up() {
    peer "grep -qw swift /proc/sys/net/ipv4/tcp_available_congestion_control || \
          { nohup sudo $SWIFTCTL </dev/null >/tmp/gswift_peer_swiftctl.log 2>&1 & \
            echo \$! > /tmp/gswift_peer_swiftctl.pid; sleep 1; }"
    peer grep -qw swift /proc/sys/net/ipv4/tcp_available_congestion_control ||
        die "could not register swift on $PEER (see /tmp/gswift_peer_swiftctl.log there)"
    peer "$ALLOW_CC"
}
peer_swift_down() {
    peer 'sudo pkill -INT -x swiftctl 2>/dev/null; rm -f /tmp/gswift_peer_swiftctl.pid' || true
}

ensure_iperf_server() {
    peer_swift_up
    peer "pgrep -x iperf3 >/dev/null || iperf3 -s -D -p $IPERF_PORT"
}

cleanup_all() {
    stop_swift
    peer_swift_down
    probe_server_stop
    bn_down
}
