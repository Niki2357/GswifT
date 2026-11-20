#!/usr/bin/env bash
# E11: how much delay does the emulated bottleneck itself add? Swift target
# sweep, 8 flows, under three conditions:
#   nobn  no bottleneck (host and wire only)
#   ifb   the standard bottleneck (receiver ingress -> IFB -> HTB + FIFO)
#   veth  receiver apps in a netns behind veth, HTB + FIFO on veth egress
# Targets at which ifb or veth fall below nobn are where the emulator's own
# latency, not the algorithm, limits Swift.
#
#   ./scripts/run_e11_emulator.sh                (about 15 minutes at REPS=3)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
MODES="${MODES:-nobn ifb veth}"
TARGETS="${TARGETS:-50 75 100 150 200 300}"
DUR="${DUR:-10}"
REPS="${REPS:-3}"
veth_down() { peer "sudo $REPO/scripts/rxns.sh unshape" >/dev/null 2>&1 || true; }
trap 'veth_down; cleanup_all' EXIT

peer "sudo $REPO/scripts/rxns.sh up" >/dev/null
sudo ip route replace 10.10.2.0/24 via 10.10.1.2 dev "$IFACE" quickack 1
for REP in $(rep_list); do
    for mode in $MODES; do
        export RX_NS="" PEER_IP=10.10.1.2 QMON_DEV=ifb0
        case "$mode" in
            nobn) bn_down; veth_down ;;
            ifb)  veth_down; bn_up ;;
            veth) bn_down; peer "sudo $REPO/scripts/rxns.sh shape $BN_RATE $BN_LIMIT $BN_ECN_US" >/dev/null
                  export RX_NS=rx PEER_IP=10.10.2.2 QMON_DEV=vh ;;
        esac
        for t in $TARGETS; do
            d="$(run_dir e11_emulator "$mode-t$t$(rep_suffix "$REP")")"
            run_bulk "$d" swift 8 "$DUR" --base-target "$t"
            printf 'emulator=%s\nbase_target_us=%s\n' "$mode" "$t" >> "$d/meta.txt"
        done
    done
done
bn_down; veth_down
log "E11 done"
