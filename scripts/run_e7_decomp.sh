#!/usr/bin/env bash
# E7 (RQ1): where does the RTT a Linux sender measures come from?
# Captures headers on BOTH nodes during a bulk run, alongside the module's
# per-ACK trace, so each RTT can be split without clock sync:
#   receiver turnaround = ACK leaves receiver - data reached receiver (rx pcap)
#   wire + queue        = ACK reached sender - data left sender (tx pcap)
#                         minus the receiver turnaround
#   sender stack        = module RTT sample - tx pcap RTT
# (analysis/decomp.py does the matching.)
#
#   ./scripts/run_e7_decomp.sh
#   CONFIGS="nobn:1:2G bn:8:max" VARIANT=swift ./scripts/run_e7_decomp.sh
#
# CONFIGS entries are <bottleneck: nobn|bn>:<flows>:<iperf rate or max>.
# Runs are short (DUR, default 4 s) to keep the captures small.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
DUR="${DUR:-4}"
CONFIGS="${CONFIGS:-nobn:1:1G nobn:1:3G nobn:1:6G nobn:1:max nobn:8:max bn:8:max}"
VARIANT="${VARIANT:-swift}"
TARGET="${TARGET:-100000}"   # unreachable by default: Swift never backs off
TAG="${TAG:-}"
CAP_PEER=/tmp/gswift_rx.pcap

stop_tcpdump() { sudo pkill -INT -x tcpdump 2>/dev/null || true; }
peer_stop_tcpdump() { peer 'sudo pkill -INT -x tcpdump 2>/dev/null; sleep 1' || true; }
trap 'stop_tcpdump; peer_stop_tcpdump; cleanup_all' EXIT

for cfg in $CONFIGS; do
    IFS=: read -r bn flows rate <<< "$cfg"
    d="$(run_dir e7_decomp "$VARIANT-$bn-n$flows-$rate$TAG")"
    if [[ "$bn" == bn ]]; then bn_up; else bn_down; fi
    if [[ "$rate" == max ]]; then extra=""; else extra="-b $(( ${rate%G} * 1000 / flows ))M"; fi
    # Snap 96 bytes: headers only. Big buffers so the capture keeps up.
    sudo tcpdump -i "$IFACE" -s 96 -B 262144 -n -w "$d/tx.pcap" "tcp port $IPERF_PORT" \
        </dev/null >"$d/tcpdump_tx.log" 2>&1 &
    peer "nohup sudo tcpdump -i \$(ip -br addr | awk '\$3 ~ /^10\\.10\\.1\\./ {print \$1; exit}') \
          -s 96 -B 262144 -n -w $CAP_PEER 'tcp port $IPERF_PORT' </dev/null >/tmp/gswift_tcpdump_rx.log 2>&1 &"
    sleep 1.5
    IPERF_EXTRA="$extra" run_bulk "$d" "$VARIANT" "$flows" "$DUR" --base-target "$TARGET" --trace-every 1
    printf 'bottleneck=%s\noffered=%s\n' "$bn" "$rate" >> "$d/meta.txt"
    stop_tcpdump
    peer_stop_tcpdump
    scp -q -o BatchMode=yes "$PEER:$CAP_PEER" "$d/rx.pcap" || log "no rx capture"
    peer "cat /tmp/gswift_tcpdump_rx.log" > "$d/tcpdump_rx.log" 2>&1 || true
    sleep 1
done
bn_down
log "E7 done"
