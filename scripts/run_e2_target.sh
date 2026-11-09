#!/usr/bin/env bash
# E2 (RQ1): sweep Swift's base target delay. How low can the target go before
# software RTT noise makes Swift under-utilize the link?
#
#   ./scripts/run_e2_target.sh           (about 4 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap cleanup_all EXIT
DUR="${DUR:-15}"
TARGETS="${TARGETS:-25 50 75 100 150 200 300 500}"
FLOWS="${FLOWS:-8}"

bn_up
for t in $TARGETS; do
    d="$(run_dir e2_target "t$t")"
    run_bulk "$d" swift "$FLOWS" "$DUR" --base-target "$t"
    echo "base_target_us=$t" >> "$d/meta.txt"
done
bn_down
log "E2 done"
