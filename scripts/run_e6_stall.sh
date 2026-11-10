#!/usr/bin/env bash
# E6 (diagnostic): where do fractional-window incast flows lose ~200 ms?
# Short incast runs with every ACK traced and a header capture on the sender,
# comparing the 1 ms gap cap variant with the one-packet floor.
#
#   ./scripts/run_e6_stall.sh            (about 3 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
VARIANTS="${VARIANTS:-swift-pcap swift-nofrac}"
FANIN="${FANIN:-64}"
ROUNDS="${ROUNDS:-20}"
LIMIT="${LIMIT:-100}"
TDPID=""
# sudo ignores signals from its own process group: signal tcpdump itself.
stop_tcpdump() { sudo pkill -INT -x tcpdump 2>/dev/null || true; }
trap 'stop_tcpdump; cleanup_all' EXIT

bn_up "$BN_RATE" "$LIMIT" "$BN_ECN_US"
for v in $VARIANTS; do
    d="$(run_dir e6_stall "$v-n$FANIN")"
    # Headers only, both directions of the incast connections.
    sudo tcpdump -i "$IFACE" -s 96 -B 65536 -w "$d/sender.pcap" "tcp port $INCAST_PORT" \
        </dev/null >/dev/null 2>"$d/tcpdump.log" &
    TDPID=$!
    sleep 1
    SWIFT_BASE_ARGS="--base-target 100 --trace-every 1" \
        run_incast "$d" "$v" "$FANIN" 65536 "$ROUNDS"
    stop_tcpdump
    wait "$TDPID" 2>/dev/null || true
    TDPID=""
    echo "bn_limit=$LIMIT" >> "$d/meta.txt"
done
bn_down
log "E6 done"
