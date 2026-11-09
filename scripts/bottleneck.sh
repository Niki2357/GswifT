#!/usr/bin/env bash
# Emulated bottleneck on the RECEIVER's ingress: all IPv4 traffic arriving on
# the experiment interface is redirected through ifb0, rate-limited by HTB, and
# queued in a single FIFO of LIMIT packets. Packets whose sojourn time exceeds
# ECN_US get CE-marked if ECN-capable (only DCTCP is), so every algorithm sees
# the same buffer.
#
#   sudo bottleneck.sh up 10gbit 300 50     rate, buffer (packets), mark (us)
#   sudo bottleneck.sh down
#   sudo bottleneck.sh show

set -euo pipefail

IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}"
IFB=ifb0
[[ -n "$IFACE" ]] || { echo "no 10.10.1.x interface" >&2; exit 1; }

down() {
    tc qdisc del dev "$IFACE" ingress 2>/dev/null || true
    tc qdisc del dev "$IFB" root 2>/dev/null || true
}

up() {
    local rate="$1" limit="$2" ecn_us="$3"
    [[ "$limit" =~ ^[0-9]+$ && "$limit" -gt 0 ]] || { echo "bad limit '$limit'" >&2; exit 1; }
    [[ "$ecn_us" =~ ^[0-9]+$ ]] || { echo "bad ecn threshold '$ecn_us'" >&2; exit 1; }
    down
    modprobe ifb numifbs=1
    ip link show "$IFB" >/dev/null 2>&1 || ip link add "$IFB" type ifb
    ip link set "$IFB" mtu "$(cat /sys/class/net/"$IFACE"/mtu)" up

    tc qdisc add dev "$IFACE" handle ffff: ingress
    tc filter add dev "$IFACE" parent ffff: protocol ip u32 match u32 0 0 \
        action mirred egress redirect dev "$IFB"

    tc qdisc add dev "$IFB" root handle 1: htb default 10
    tc class add dev "$IFB" parent 1: classid 1:10 htb rate "$rate" ceil "$rate" \
        burst 256k cburst 256k
    # fq_codel with one flow queue = a single FIFO. CoDel's drop law is pushed
    # out of reach (target 1s) so overflow at LIMIT is the only drop cause.
    local ce=""
    [[ "$ecn_us" -gt 0 ]] && ce="ce_threshold ${ecn_us}us"
    tc qdisc add dev "$IFB" parent 1:10 handle 10: fq_codel flows 1 \
        limit "$limit" target 1s interval 2s drop_batch 1 ecn $ce
    echo "bottleneck up: $IFACE ingress -> $IFB, rate $rate, buffer $limit pkts, CE ${ecn_us}us"
}

case "${1:-}" in
    up)   up "${2:?rate}" "${3:?limit}" "${4:-0}" ;;
    down) down; echo "bottleneck down" ;;
    show) tc -s qdisc show dev "$IFB"; tc -s class show dev "$IFB" ;;
    *)    echo "usage: $0 up RATE LIMIT_PKTS [ECN_US] | down | show" >&2; exit 2 ;;
esac
