#!/usr/bin/env bash
# E0 (RQ1): RTT measurement noise floor. No bottleneck, so any RTT above the
# unloaded minimum is host/stack delay, not network queuing. Swift is loaded
# with an unreachable target so it never backs off; we only read its samples.
#
#   ./scripts/run_e0_noise.sh            (about 1 minute)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap cleanup_all EXIT
bn_down
DUR="${DUR:-15}"

for load in 1G 5G max; do
    d="$(run_dir e0_noise "load-$load")"
    if [[ "$load" == max ]]; then IPERF_EXTRA=""; else IPERF_EXTRA="-b $load"; fi
    IPERF_EXTRA="$IPERF_EXTRA" run_bulk "$d" swift 1 "$DUR" --base-target 100000 --trace-every 1
    echo "offered_load=$load" >> "$d/meta.txt"
done
log "E0 done"
