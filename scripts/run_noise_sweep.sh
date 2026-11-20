#!/usr/bin/env bash
# Phase 2/3: does host noise explain Swift's drift at a low target, and does a
# floor-relative target fix it? For each noise level (applied on both nodes),
# runs an interleaved block (scripts/run_block.sh) tagged -<level>.
#
#   ./scripts/run_noise_sweep.sh
#   LEVELS="none cpu50 cpu100" REPS=3 TARGETS="100 200" \
#     BASELINES="swift-floor60 cubic dctcp" ./scripts/run_noise_sweep.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
LEVELS="${LEVELS:-none cpu25 cpu50 cpu75 cpu100}"
export REPS="${REPS:-3}" TARGETS="${TARGETS:-100 200}"
export BASELINES="${BASELINES:-cubic dctcp}"

noise() { sudo "$REPO/scripts/noise.sh" "$@"; peer sudo "$REPO/scripts/noise.sh" "$@"; }
trap 'noise down >/dev/null 2>&1 || true' EXIT

for lvl in $LEVELS; do
    noise up "$lvl"
    TAG="-$lvl" EXTRA_META="noise=$lvl" "$REPO/scripts/run_block.sh" ||
        log "block failed at noise $lvl"
    noise down >/dev/null
done
log "noise sweep done"
