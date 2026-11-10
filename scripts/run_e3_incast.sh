#!/usr/bin/env bash
# E3 (RQ3): fan-in sweep. N synchronized flows each send BYTES per round into
# a small buffer. Compares Swift with and without the fractional (< 1 packet)
# window against Cubic and DCTCP.
#
#   ./scripts/run_e3_incast.sh           (about 8 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
FANIN="${FANIN:-16 32 64 128 256 512}"
VARIANTS="${VARIANTS:-swift swift-nofrac cubic dctcp}"
BYTES="${BYTES:-65536}"
ROUNDS="${ROUNDS:-100}"
LIMIT="${LIMIT:-100}"
# RTO_MIN=5ms lowers the sender's minimum RTO on the experiment route, for
# every algorithm alike. It also shortens Linux's tail loss probe when one
# packet is in flight (PTO adds rto_min then), which is what stalls
# sub-packet windows for ~200 ms. Restored on exit.
RTO_MIN="${RTO_MIN:-}"
ROUTE0=""
rto_up() {
    [[ -n "$RTO_MIN" ]] || return 0
    ROUTE0="$(ip route show dev "$IFACE" | grep -m1 '^10\.10\.1\.')"
    # shellcheck disable=SC2086
    sudo ip route replace $(sed 's/ rto_min [^ ]*//' <<< "$ROUTE0") dev "$IFACE" rto_min "$RTO_MIN"
    log "route: $(ip route show dev "$IFACE" | grep -m1 '^10\.10\.1\.')"
}
rto_down() {
    [[ -n "$ROUTE0" ]] || return 0
    # shellcheck disable=SC2086
    sudo ip route replace $ROUTE0 dev "$IFACE" || true
    ROUTE0=""
}
trap 'rto_down; cleanup_all' EXIT
tag="${RTO_MIN:+-rto$RTO_MIN}"
rto_up

bn_up "$BN_RATE" "$LIMIT" "$BN_ECN_US"
for REP in $(rep_list); do
    for n in $FANIN; do
        for v in $VARIANTS; do
            d="$(run_dir e3_incast "$v-n$n$tag$(rep_suffix "$REP")")"
            SWIFT_BASE_ARGS="--base-target 100 --trace-every 16" \
                run_incast "$d" "$v" "$n" "$BYTES" "$ROUNDS"
            printf 'bn_limit=%s\nrto_min=%s\n' "$LIMIT" "${RTO_MIN:-default}" >> "$d/meta.txt"
        done
    done
done
bn_down
rto_down
log "E3 done"
