#!/usr/bin/env bash
# Receiver network namespace for a veth-based bottleneck (run on node1).
#
# The receiver applications live in netns "rx" (10.10.2.2) behind a veth pair;
# node1 forwards 10.10.1.x <-> 10.10.2.x, and the bottleneck is an ordinary
# egress qdisc on the host-side veth "vh". Unlike the IFB bottleneck there is
# no tasklet hop on the receive path (E11 compares the two).
#
#   sudo rxns.sh up                       create netns, veth, addresses, sysctls
#   sudo rxns.sh shape 10gbit 300 50      HTB + byte-bounded fq_codel FIFO on vh
#   sudo rxns.sh unshape
#   sudo rxns.sh down
#
# node0 needs: ip route replace 10.10.2.0/24 via 10.10.1.2 quickack 1

set -euo pipefail
NS=rx
MEM_PER_PKT="${BN_MEM_PER_PKT:-2180}"

up() {
    ip netns add "$NS" 2>/dev/null || true
    ip link show vh >/dev/null 2>&1 || ip link add vh type veth peer name vr
    ip link set vr netns "$NS" 2>/dev/null || true
    ip addr replace 10.10.2.1/24 dev vh
    ip link set vh up
    ip netns exec "$NS" ip addr replace 10.10.2.2/24 dev vr
    ip netns exec "$NS" ip link set vr up
    ip netns exec "$NS" ip link set lo up
    ip netns exec "$NS" ip route replace default via 10.10.2.1 quickack 1
    sysctl -qw net.ipv4.ip_forward=1
    # Per-namespace TCP settings matching the root namespace's experiment setup.
    ip netns exec "$NS" sysctl -qw net.ipv4.tcp_ecn=1 \
        net.ipv4.tcp_slow_start_after_idle=0 \
        net.ipv4.tcp_rmem="4096 131072 268435456" net.ipv4.tcp_wmem="4096 131072 268435456"
    echo "rx netns up: 10.10.2.2 behind vh"
}

shape() {
    local rate="$1" limit="$2" ecn_us="$3" ce=""
    [[ "$ecn_us" -gt 0 ]] && ce="ce_threshold ${ecn_us}us"
    tc qdisc del dev vh root 2>/dev/null || true
    tc qdisc add dev vh root handle 1: htb default 10
    tc class add dev vh parent 1: classid 1:10 htb rate "$rate" ceil "$rate" \
        burst "${BN_BURST:-1m}" cburst "${BN_BURST:-1m}"
    # shellcheck disable=SC2086
    tc qdisc add dev vh parent 1:10 handle 10: fq_codel flows 1 limit 100000 \
        memory_limit $(( limit * MEM_PER_PKT )) target 1s interval 2s drop_batch 1 ecn $ce
    echo "veth bottleneck: vh rate $rate, buffer $limit pkts, CE ${ecn_us}us"
}

case "${1:-}" in
    up)      up ;;
    shape)   shape "${2:?rate}" "${3:?limit}" "${4:-0}" ;;
    unshape) tc qdisc del dev vh root 2>/dev/null || true; echo "veth bottleneck off" ;;
    down)    ip netns del "$NS" 2>/dev/null || true; ip link del vh 2>/dev/null || true; echo "rx netns down" ;;
    *)       echo "usage: $0 up | shape RATE LIMIT [ECN_US] | unshape | down" >&2; exit 2 ;;
esac
