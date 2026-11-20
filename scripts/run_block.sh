#!/usr/bin/env bash
# Interleaved E1/E2 block at 8 flows: each repeat measures the unloaded host
# RTT, then runs every Swift target and the baselines back to back, so all
# configurations see the same host state. Swift at a low target drifted
# within one session on 2026-11-10; this layout makes that visible.
#
#   ./scripts/run_block.sh                          (about 2.5 min per repeat)
#   REPS=5 TARGETS="100 200" ./scripts/run_block.sh
#   TAG=-noise2 ./scripts/run_block.sh              (suffix for run names)
#
# Writes e2_target/t<T><TAG>-r<k>, e1_compare/<v>-n<F><TAG>-r<k> and
# host/<TAG>-r<k>/baseline.txt.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap cleanup_all EXIT
DUR="${DUR:-15}"
TARGETS="${TARGETS:-100 150 200 300}"
BASELINES="${BASELINES:-cubic dctcp}"
FLOWS="${FLOWS:-8}"
TAG="${TAG:-}"
EXTRA_META="${EXTRA_META:-}"   # extra key=value lines for meta.txt

add_meta() { [[ -z "$EXTRA_META" ]] || printf '%s\n' $EXTRA_META >> "$1/meta.txt"; }

for REP in $(rep_list); do
    h="$(run_dir host "base${TAG}$(rep_suffix "$REP")")"
    host_baseline "$h"
    bn_up
    for t in $TARGETS; do
        d="$(run_dir e2_target "t$t$TAG$(rep_suffix "$REP")")"
        run_bulk "$d" swift "$FLOWS" "$DUR" --base-target "$t"
        echo "base_target_us=$t" >> "$d/meta.txt"
        add_meta "$d"
    done
    for v in $BASELINES; do
        d="$(run_dir e1_compare "$v-n$FLOWS$TAG$(rep_suffix "$REP")")"
        run_bulk "$d" "$v" "$FLOWS" "$DUR"
        add_meta "$d"
    done
done
bn_down
log "block done"
