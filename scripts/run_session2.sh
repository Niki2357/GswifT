#!/usr/bin/env bash
# Second evaluation session: repeats of the key results, E4 redone so it tests
# MSwift, and the sub-packet pacing variants (swift-pmin, swift-pcap).
# See RUNNING.md "Next session plan". About 2 hours at REPS=5.
#
#   ./scripts/run_session2.sh            (from node0, logged in with ssh -A)
#   REPS=3 ./scripts/run_session2.sh     (shorter)
#
# Each step runs even if an earlier one failed; failures are listed at the end.
set -uo pipefail
S="$(dirname "${BASH_SOURCE[0]}")"
export REPS="${REPS:-5}"
FAILED=()
step() {  # step <name> [VAR=value ...] <script> [args...]
    local name="$1"; shift
    echo "=== $name ($(date +%H:%M:%S))"
    env "$@" || FAILED+=("$name")
}

"$S/session_info.sh" || FAILED+=(session_info)
# New hardware, so re-measure the noise floor once (about 1 minute).
step e0  REPS=1 "$S/run_e0_noise.sh"
step e1  FLOWS="1 8" VARIANTS="swift cubic dctcp" "$S/run_e1_compare.sh"
step e2  TARGETS="100 200 300" "$S/run_e2_target.sh"
step e3  FANIN="64 128" VARIANTS="swift swift-nofrac swift-pmin swift-pcap cubic" \
         "$S/run_e3_incast.sh"
E4_REPS=$(( REPS < 3 ? REPS : 3 ))
step e4         REPS="$E4_REPS" "$S/run_e4_spray.sh"
step e4-reorder REPS="$E4_REPS" REORDER_TOL=64 "$S/run_e4_spray.sh"

source "$S/common.sh"
tar -C "$OUT_ROOT" -czf "$OUT_ROOT/results-$DATE.tgz" "$DATE"
echo "Bundle: $OUT_ROOT/results-$DATE.tgz"
if (( ${#FAILED[@]} )); then echo "FAILED steps: ${FAILED[*]}"; exit 1; fi
echo "All steps done."
