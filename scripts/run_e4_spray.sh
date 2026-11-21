#!/usr/bin/env bash
# E4 (MSwift): fraction q of packets takes a slower path (+DELAY_US). Baseline
# Swift reacts to every slow sample; MSwift's median filter should not, until
# q approaches 1/2. Bottleneck stays on so throughput has a ceiling.
#
# Spraying needs TSO/GSO off, which capped the sender near 4 Gb/s on
# 2026-11-09, so the bottleneck defaults to 2 Gb/s here (E4_BN_RATE).
#
# REORDER_TOL=N makes Linux loss recovery tolerate reordering: RACK off,
# dupthresh N. Without it, sprayed packets trigger fast recovery, whose cwnd
# cut bypasses the median filter. Original sysctls are restored on exit.
#
# SPRAY_AT=rx moves the path choice to the receiver's bottleneck, after
# receive offloads with GRO off (bottleneck.sh spray mode), so the choice is
# per wire packet and the sender keeps TSO/GSO. GRO off limits the receiver's
# IFB to ~2 Gb/s, so the bottleneck defaults to 1 Gb/s. DELAYS sweeps the
# slow-path delay.
#
#   ./scripts/run_e4_spray.sh                       (about 9 minutes)
#   REORDER_TOL=64 ./scripts/run_e4_spray.sh
#   SPRAY_AT=rx DELAYS="100 300 1000" REORDER_TOL=64 ./scripts/run_e4_spray.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
DUR="${DUR:-15}"
QS="${QS:-0 0.05 0.1 0.25 0.5}"
VARIANTS="${VARIANTS:-swift mswift mswift-k4 cubic}"
SPRAY_AT="${SPRAY_AT:-tx}"
DELAYS="${DELAYS:-${DELAY_US:-300}}"
FLOWS="${FLOWS:-8}"
if [[ "$SPRAY_AT" == rx ]]; then BN_RATE="${E4_BN_RATE:-1gbit}"; else BN_RATE="${E4_BN_RATE:-2gbit}"; fi
REORDER_TOL="${REORDER_TOL:-}"

RO_KEYS="net.ipv4.tcp_recovery net.ipv4.tcp_reordering net.ipv4.tcp_max_reordering"
RO_SAVED=""
reorder_up() {
    [[ -n "$REORDER_TOL" ]] || return 0
    local k
    for k in $RO_KEYS; do RO_SAVED+="$k=$(sysctl -n "$k") "; done
    local max=$(( REORDER_TOL > 300 ? REORDER_TOL : 300 ))
    sudo sysctl -qw net.ipv4.tcp_recovery=0 net.ipv4.tcp_reordering="$REORDER_TOL" \
        net.ipv4.tcp_max_reordering="$max"
    log "reordering tolerance on: was $RO_SAVED"
}
reorder_down() {
    [[ -n "$RO_SAVED" ]] || return 0
    # shellcheck disable=SC2086
    sudo sysctl -qw $RO_SAVED || true
    RO_SAVED=""
}
spray_down() { sudo "$REPO/scripts/spray.sh" down >/dev/null || true; }
trap 'spray_down; reorder_down; cleanup_all' EXIT

tag="${REORDER_TOL:+-ro$REORDER_TOL}"
[[ "$SPRAY_AT" == rx ]] && tag="$tag-rx"
[[ "$FLOWS" == 8 ]] || tag="$tag-f$FLOWS"
multi_delay=0; [[ "$DELAYS" == *" "* ]] && multi_delay=1

# Sets up the path split for one run. tx: sender iptables + netem (counters
# reset per run). rx: the receiver's bottleneck is rebuilt with spraying.
spray_for_run() {  # spray_for_run <q> <delay_us>
    if [[ "$SPRAY_AT" == rx ]]; then
        if [[ "$1" == 0 ]]; then bn_up; else
            peer sudo BN_BURST="${BN_BURST:-1m}" "$REPO/scripts/bottleneck.sh" up "$BN_RATE" "$BN_LIMIT" "$BN_ECN_US" \
                "$1" "$2" "$IPERF_PORT"
        fi
    else
        sudo "$REPO/scripts/spray.sh" up "$1" "$2" "$IPERF_PORT" >/dev/null
    fi
}
spray_counters() {  # spray_counters <dir>
    if [[ "$SPRAY_AT" == rx ]]; then cp "$1/tc_final.txt" "$1/spray_counters.txt"
    else sudo "$REPO/scripts/spray.sh" show > "$1/spray_counters.txt" 2>&1; fi
}

reorder_up
[[ "$SPRAY_AT" == rx ]] || bn_up
for REP in $(rep_list); do
    for dly in $DELAYS; do
        dtag=""; (( multi_delay )) && dtag="-d$dly"
        for q in $QS; do
            for v in $VARIANTS; do
                spray_for_run "$q" "$dly"
                d="$(run_dir e4_spray "$v-q$q$dtag$tag$(rep_suffix "$REP")")"
                run_bulk "$d" "$v" "$FLOWS" "$DUR"
                printf 'q=%s\nspray_delay_us=%s\nreorder_tol=%s\nspray_at=%s\n' \
                    "$q" "$dly" "${REORDER_TOL:-default}" "$SPRAY_AT" >> "$d/meta.txt"
                spray_counters "$d"
            done
        done
    done
done
spray_down
bn_down
reorder_down
log "E4 done"
