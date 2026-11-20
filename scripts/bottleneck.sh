#!/usr/bin/env bash
# Emulated bottleneck on the RECEIVER's ingress: all IPv4 traffic arriving on
# the experiment interface is redirected through ifb0, rate-limited by HTB, and
# queued in a single FIFO of LIMIT packets. Packets whose sojourn time exceeds
# ECN_US get CE-marked if ECN-capable (only DCTCP is), so every algorithm sees
# the same buffer.
#
#   sudo bottleneck.sh up 10gbit 300 50     rate, buffer (packets), mark (us)
#   sudo bottleneck.sh up 10gbit 300 50 0.25 300 5201
#        ... plus spraying: a random fraction q (0.25) of TCP data packets to
#        port 5201 takes a slow path adding 300 us (netem), chosen per wire
#        packet by tools/spray/spray_cls.bpf.c. GRO is turned off so every
#        wire packet is a separate choice. The slow path shares the rate limit
#        but not the FIFO.
#   sudo bottleneck.sh down
#   sudo bottleneck.sh show

set -euo pipefail

IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}"
IFB=ifb0
[[ -n "$IFACE" ]] || { echo "no 10.10.1.x interface" >&2; exit 1; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPRAY_OBJ=/tmp/gswift_spray_cls.o

down() {
    tc qdisc del dev "$IFACE" ingress 2>/dev/null || true
    tc qdisc del dev "$IFB" root 2>/dev/null || true
    if [[ -f /tmp/gswift_gro_was_on ]]; then
        ethtool -K "$IFACE" gro on >/dev/null 2>&1 || true
        rm -f /tmp/gswift_gro_was_on
    fi
}

# Compiles the classifier with probability q baked in.
build_spray() {  # build_spray <q> <port>
    local thresh
    thresh="$(python3 -c "q=float('$1'); assert 0 <= q <= 1; print(min(int(q * 2**32), 2**32 - 1))")" ||
        { echo "q must be in [0,1]" >&2; exit 1; }
    clang -O2 -g -target bpf -I"/usr/include/$(uname -m)-linux-gnu" \
        -DTHRESH="${thresh}U" -DDPORT_LO="$2" -DDPORT_HI="$(( $2 + 63 ))" -DSLOW_CLASSID=0x20001 \
        -c "$REPO/tools/spray/spray_cls.bpf.c" -o "$SPRAY_OBJ"
}

up() {
    local rate="$1" limit="$2" ecn_us="$3" q="${4:-}" delay_us="${5:-300}" port="${6:-5201}"
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
    local fifo_parent=1:10
    if [[ -n "$q" ]]; then
        [[ "$delay_us" =~ ^[0-9]+$ && "$port" =~ ^[0-9]+$ ]] ||
            { echo "bad spray delay/port" >&2; exit 1; }
        build_spray "$q" "$port"
        if ethtool -k "$IFACE" | grep -q '^generic-receive-offload: on'; then
            touch /tmp/gswift_gro_was_on
            ethtool -K "$IFACE" gro off
        fi
        # Band 0 (2:1): slow path, served first once its delay has elapsed.
        # Band 1 (2:2): the shared FIFO, where everything unclassified goes.
        tc qdisc add dev "$IFB" parent 1:10 handle 2: prio bands 2 \
            priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        tc qdisc add dev "$IFB" parent 2:1 handle 20: netem delay "${delay_us}us" limit 100000
        tc filter add dev "$IFB" parent 2: protocol ip prio 1 bpf obj "$SPRAY_OBJ" sec classifier
        fifo_parent=2:2
    fi
    tc qdisc add dev "$IFB" parent "$fifo_parent" handle 10: fq_codel flows 1 \
        limit "$limit" target 1s interval 2s drop_batch 1 ecn $ce
    echo "bottleneck up: $IFACE ingress -> $IFB, rate $rate, buffer $limit pkts, CE ${ecn_us}us${q:+, spray q=$q +${delay_us}us port $port (GRO off)}"
}

case "${1:-}" in
    up)   up "${2:?rate}" "${3:?limit}" "${4:-0}" "${5:-}" "${6:-300}" "${7:-5201}" ;;
    down) down; echo "bottleneck down" ;;
    show) tc -s qdisc show dev "$IFB"; tc -s class show dev "$IFB" ;;
    *)    echo "usage: $0 up RATE LIMIT_PKTS [ECN_US [SPRAY_Q [DELAY_US [PORT]]]] | down | show" >&2; exit 2 ;;
esac
