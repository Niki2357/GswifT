#!/usr/bin/env bash
# Session 3 follow-ups (2026-11-20), after run_session3.sh. About 90 minutes.
#   STEPS="e11 jitter e4win e8veth" ./scripts/run_followups.sh
set -uo pipefail
S="$(dirname "${BASH_SOURCE[0]}")"
STEPS="${STEPS:-e11 jitter e4win e8veth}"
FAILED=()
step() {
    local name="$1"; shift
    [[ " $STEPS " == *" $name "* ]] || return 0
    echo "=== $name ($(date +%H:%M:%S))"
    env "$@" || FAILED+=("$name")
}
# Emulator fidelity: where does the bottleneck's own delay take over?
step e11 REPS=3 "$S/run_e11_emulator.sh"
# De-synchronization: fixed targets vs per-flow jitter with the same mean.
step jitter REPS=5 TARGETS="100 150" TAG=-jit \
    BASELINES="swift-t80j40 swift-t130j40 swift-t110j80" "$S/run_block.sh"
# MSwift's median needs windows of several packets: 1 and 2 flows.
for f in 1 2; do
    step e4win REPS=2 SPRAY_AT=rx REORDER_TOL=64 FLOWS=$f QS="0 0.1 0.25 0.35 0.5" \
        DELAYS="300 1000" VARIANTS="swift mswift mswift-k4" "$S/run_e4_spray.sh"
done
# RPC workloads on the veth bottleneck (no IFB per-packet ceiling).
if [[ " $STEPS " == *" e8veth "* ]]; then
    # shellcheck disable=SC1091
    source "$S/use_veth.sh"
    step e8veth REPS=2 TAG=-veth VARIANTS="swift-t200 swift-t300 dctcp cubic" "$S/run_e8_workload.sh"
fi
if (( ${#FAILED[@]} )); then echo "FAILED steps: ${FAILED[*]}"; exit 1; fi
echo "All steps done ($(date +%H:%M:%S))."
