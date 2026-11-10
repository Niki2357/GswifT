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
#   ./scripts/run_e4_spray.sh                       (about 9 minutes)
#   REORDER_TOL=64 ./scripts/run_e4_spray.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
DUR="${DUR:-15}"
QS="${QS:-0 0.05 0.1 0.25 0.5}"
VARIANTS="${VARIANTS:-swift mswift mswift-k4 cubic}"
DELAY_US="${DELAY_US:-300}"
FLOWS="${FLOWS:-8}"
BN_RATE="${E4_BN_RATE:-2gbit}"
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
reorder_up
bn_up
for REP in $(rep_list); do
    for q in $QS; do
        sudo "$REPO/scripts/spray.sh" up "$q" "$DELAY_US" "$IPERF_PORT"
        for v in $VARIANTS; do
            d="$(run_dir e4_spray "$v-q$q$tag$(rep_suffix "$REP")")"
            run_bulk "$d" "$v" "$FLOWS" "$DUR"
            printf 'q=%s\nspray_delay_us=%s\nreorder_tol=%s\n' \
                "$q" "$DELAY_US" "${REORDER_TOL:-default}" >> "$d/meta.txt"
            sudo "$REPO/scripts/spray.sh" show > "$d/spray_counters.txt" 2>&1
            # Reset counters between variants so each run's split can be checked.
            sudo "$REPO/scripts/spray.sh" up "$q" "$DELAY_US" "$IPERF_PORT" >/dev/null
        done
    done
done
spray_down
bn_down
reorder_down
log "E4 done"
