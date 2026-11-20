#!/usr/bin/env bash
# E10 (RQ1): how much of the load-dependent host delay can host settings
# remove? No bottleneck; Swift with an unreachable target (never backs off)
# reports every RTT sample. Each knob is applied, measured at each load, and
# reverted.
#
#   ./scripts/run_e10_knobs.sh                 (about 8 minutes)
#   KNOBS="base tso_off" LOADS="1:3G" ./scripts/run_e10_knobs.sh
#
# Knobs (sender = node0, receiver = node1):
#   base        no change
#   tso_off     sender TSO and GSO off (wire-size segments through the stack)
#   tsq_small   sender tcp_limit_output_bytes 1 MB -> 128 KB (TCP Small Queues)
#   bql_small   sender byte queue limit 64 KB per TX queue (bytes in the NIC ring)
#   gro_off     receiver GRO off
#   busypoll    busy_poll/busy_read 50 us on both nodes

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
KNOBS="${KNOBS:-base tso_off tsq_small bql_small gro_off busypoll}"
LOADS="${LOADS:-1:3G 1:max 8:max}"
DUR="${DUR:-5}"
TAG="${TAG:-}"

bql_set() {  # bql_set <bytes|max>
    local q v="$1"
    [[ "$v" == max ]] && v=1879048192
    for q in /sys/class/net/"$IFACE"/queues/tx-*/byte_queue_limits/limit_max; do
        echo "$v" | sudo tee "$q" >/dev/null
    done
}
knob() {  # knob <name> <up|down>
    case "$1:$2" in
        base:*) ;;
        tso_off:up)   sudo ethtool -K "$IFACE" tso off gso off ;;
        tso_off:down) sudo ethtool -K "$IFACE" tso on gso on ;;
        tsq_small:up)   sudo sysctl -qw net.ipv4.tcp_limit_output_bytes=131072 ;;
        tsq_small:down) sudo sysctl -qw net.ipv4.tcp_limit_output_bytes=1048576 ;;
        bql_small:up)   bql_set 65536 ;;
        bql_small:down) bql_set max ;;
        gro_off:up)   peer "sudo ethtool -K $IFACE gro off" ;;
        gro_off:down) peer "sudo ethtool -K $IFACE gro on" ;;
        busypoll:up)
            sudo sysctl -qw net.core.busy_poll=50 net.core.busy_read=50
            peer sudo sysctl -qw net.core.busy_poll=50 net.core.busy_read=50 ;;
        busypoll:down)
            sudo sysctl -qw net.core.busy_poll=0 net.core.busy_read=0
            peer sudo sysctl -qw net.core.busy_poll=0 net.core.busy_read=0 ;;
        *) die "unknown knob $1" ;;
    esac
}
CUR=""
trap '[[ -n "$CUR" ]] && knob "$CUR" down; cleanup_all' EXIT

bn_down
for k in $KNOBS; do
    CUR="$k"; knob "$k" up
    for cfg in $LOADS; do
        IFS=: read -r flows rate <<< "$cfg"
        d="$(run_dir e10_knobs "$k-n$flows-$rate$TAG")"
        if [[ "$rate" == max ]]; then extra=""; else extra="-b $(( ${rate%G} * 1000 / flows ))M"; fi
        IPERF_EXTRA="$extra" SWIFT_BASE_ARGS="--trace-every 1" \
            run_bulk "$d" swift "$flows" "$DUR" --base-target 100000
        printf 'knob=%s\noffered=%s\n' "$k" "$rate" >> "$d/meta.txt"
    done
    knob "$k" down; CUR=""
done
log "E10 done"
