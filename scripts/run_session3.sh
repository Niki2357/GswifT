#!/usr/bin/env bash
# Session 3 main queue (2026-11-20), on the fixed harness (one iperf3 process
# per flow, GRO off at the bottleneck). About 3.5 hours at the defaults.
# Each step runs even if an earlier one failed; failures are listed at the end.
#
#   ./scripts/run_session3.sh                 (from node0, logged in with ssh -A)
#   STEPS="e7 e9" ./scripts/run_session3.sh   (a subset)
set -uo pipefail
S="$(dirname "${BASH_SOURCE[0]}")"
STEPS="${STEPS:-e7 e9 block e3 e3rto e8 e4ro e4def}"
FAILED=()
step() {  # step <name> [VAR=value ...] <script> [args...]
    local name="$1"; shift
    [[ " $STEPS " == *" $name "* ]] || return 0
    echo "=== $name ($(date +%H:%M:%S))"
    env "$@" || FAILED+=("$name")
}

# RQ1: where the RTT comes from (no bottleneck: GRO on; bottleneck: GRO off).
step e7 "$S/run_e7_decomp.sh"
# Implementation costs: pacing fidelity, CPU and BPF time.
step e9 "$S/run_e9_overhead.sh"
# Steady state: every target and the floor-relative target, interleaved.
step block REPS=5 TARGETS="50 75 100 150 200 300" \
    BASELINES="cubic dctcp swift-floor60 swift-floor100" "$S/run_block.sh"
# Incast with the lone-packet fixes, then everyone with rto_min 5 ms.
step e3 REPS=3 FANIN="32 64 128 256" \
    VARIANTS="swift-nofrac swift swift-pair swift-rto swift-pairrto swift-fix cubic dctcp" \
    "$S/run_e3_incast.sh"
step e3rto REPS=3 FANIN="32 64 128 256" RTO_MIN=5ms \
    VARIANTS="swift-nofrac swift-pair cubic dctcp" "$S/run_e3_incast.sh"
# RPC workloads.
step e8 REPS=3 "$S/run_e8_workload.sh"
# MSwift against its model: per-wire-packet spraying, delay sweep.
step e4ro REPS=2 SPRAY_AT=rx REORDER_TOL=64 QS="0 0.05 0.1 0.25 0.35 0.5" \
    DELAYS="100 300 1000" "$S/run_e4_spray.sh"
step e4def REPS=2 SPRAY_AT=rx QS="0 0.05 0.1 0.25 0.35 0.5" DELAYS="300" \
    "$S/run_e4_spray.sh"

source "$S/common.sh"
if (( ${#FAILED[@]} )); then echo "FAILED steps: ${FAILED[*]}"; exit 1; fi
echo "All steps done ($(date +%H:%M:%S))."
