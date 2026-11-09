#!/usr/bin/env bash
# E5 (diagnostic): why does Swift with 8 flows under-utilize? Compares the
# default module with the fractional window disabled (min cwnd 1) and with a
# higher target, recording ss -ti snapshots (srtt, pacing rate, timers).
#
#   ./scripts/run_e5_diag.sh            (about 2 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap 'kill $SSPID 2>/dev/null; cleanup_all' EXIT
DUR="${DUR:-15}"
SSPID=""

ss_sampler() {  # ss_sampler <dir>: ss -ti snapshots every 100 ms
    ( while :; do echo "# $(date +%s.%N)"; ss -tinH "dport = :$IPERF_PORT"; sleep 0.1; done ) \
        > "$1/ss.txt" 2>&1 &
    SSPID=$!
}

bn_up
for v in swift swift-nofrac mswift; do
    d="$(run_dir e5_diag "$v-n8")"
    ss_sampler "$d"
    SWIFT_BASE_ARGS="--base-target 100 --trace-every 1" run_bulk "$d" "$v" 8 "$DUR"
    kill "$SSPID" 2>/dev/null || true
done
bn_down
log "E5 done"
