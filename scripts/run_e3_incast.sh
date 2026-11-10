#!/usr/bin/env bash
# E3 (RQ3): fan-in sweep. N synchronized flows each send BYTES per round into
# a small buffer. Compares Swift with and without the fractional (< 1 packet)
# window against Cubic and DCTCP.
#
#   ./scripts/run_e3_incast.sh           (about 8 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap cleanup_all EXIT
FANIN="${FANIN:-16 32 64 128 256 512}"
VARIANTS="${VARIANTS:-swift swift-nofrac cubic dctcp}"
BYTES="${BYTES:-65536}"
ROUNDS="${ROUNDS:-100}"
LIMIT="${LIMIT:-100}"

bn_up "$BN_RATE" "$LIMIT" "$BN_ECN_US"
for REP in $(rep_list); do
    for n in $FANIN; do
        for v in $VARIANTS; do
            d="$(run_dir e3_incast "$v-n$n$(rep_suffix "$REP")")"
            SWIFT_BASE_ARGS="--base-target 100 --trace-every 16" \
                run_incast "$d" "$v" "$n" "$BYTES" "$ROUNDS"
            echo "bn_limit=$LIMIT" >> "$d/meta.txt"
        done
    done
done
bn_down
log "E3 done"
