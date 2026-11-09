#!/usr/bin/env bash
# Two-path spraying emulation on the SENDER, for the MSwift experiment.
#
# A random fraction Q of outgoing data packets is marked (iptables statistic
# match) and sent through a netem class that adds DELAY_US, standing in for a
# congested path. The rest go straight out. This is MSwift's model: each
# packet independently takes the slow path with probability q.
#
# TSO/GSO are turned off so marking is per wire packet, not per 64 KB burst.
#
#   sudo spray.sh up 0.1 300 5201     q, extra delay (us), destination port
#   sudo spray.sh down
#   sudo spray.sh show

set -euo pipefail

IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}"
MARK=0x5
[[ -n "$IFACE" ]] || { echo "no 10.10.1.x interface" >&2; exit 1; }

down() {
    iptables -t mangle -S OUTPUT 2>/dev/null | grep -- "--set-xmark $MARK" |
        sed 's/^-A/-D/' | while read -r rule; do
            # shellcheck disable=SC2086
            iptables -t mangle $rule || true
        done
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    ethtool -K "$IFACE" tso on gso on >/dev/null 2>&1 || true
}

up() {
    local q="$1" delay_us="$2" port="$3"
    python3 -c "import sys; q=float('$q'); sys.exit(0 if 0 <= q <= 1 else 1)" ||
        { echo "q must be in [0,1]" >&2; exit 1; }
    [[ "$delay_us" =~ ^[0-9]+$ ]] || { echo "bad delay '$delay_us'" >&2; exit 1; }
    [[ "$port" =~ ^[0-9]+$ ]] || { echo "bad port '$port'" >&2; exit 1; }
    down
    ethtool -K "$IFACE" tso off gso off >/dev/null

    # Band 0: direct path. Band 1: slow path (netem). Everything defaults to 0.
    tc qdisc add dev "$IFACE" root handle 1: prio bands 2 \
        priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    tc qdisc add dev "$IFACE" parent 1:1 handle 10: pfifo limit 10000
    tc qdisc add dev "$IFACE" parent 1:2 handle 20: netem delay "${delay_us}us" limit 100000
    tc filter add dev "$IFACE" parent 1: protocol ip handle "$MARK" fw flowid 1:2

    if python3 -c "import sys; sys.exit(0 if float('$q') > 0 else 1)"; then
        iptables -t mangle -A OUTPUT -o "$IFACE" -p tcp --dport "$port" \
            -m statistic --mode random --probability "$q" -j MARK --set-xmark "$MARK"
    fi
    echo "spray up: $IFACE q=$q slow-path +${delay_us}us dport $port (TSO/GSO off)"
}

show() {
    iptables -t mangle -L OUTPUT -v -n -x
    tc -s qdisc show dev "$IFACE"
}

case "${1:-}" in
    up)   up "${2:?q}" "${3:?delay_us}" "${4:-5201}" ;;
    down) down; echo "spray down" ;;
    show) show ;;
    *)    echo "usage: $0 up Q DELAY_US [PORT] | down | show" >&2; exit 2 ;;
esac
