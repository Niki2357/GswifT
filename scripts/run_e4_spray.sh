#!/usr/bin/env bash
# E4 (MSwift): fraction q of packets takes a slower path (+DELAY_US). Baseline
# Swift reacts to every slow sample; MSwift's median filter should not, until
# q approaches 1/2. Bottleneck stays on so throughput has a ceiling.
#
#   ./scripts/run_e4_spray.sh            (about 7 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
spray_down() { sudo "$REPO/scripts/spray.sh" down >/dev/null || true; }
trap 'spray_down; cleanup_all' EXIT
DUR="${DUR:-15}"
QS="${QS:-0 0.05 0.1 0.25 0.5}"
VARIANTS="${VARIANTS:-swift mswift mswift-k4 cubic}"
DELAY_US="${DELAY_US:-300}"
FLOWS="${FLOWS:-8}"

bn_up
for q in $QS; do
    sudo "$REPO/scripts/spray.sh" up "$q" "$DELAY_US" "$IPERF_PORT"
    for v in $VARIANTS; do
        d="$(run_dir e4_spray "$v-q$q")"
        run_bulk "$d" "$v" "$FLOWS" "$DUR"
        printf 'q=%s\nspray_delay_us=%s\n' "$q" "$DELAY_US" >> "$d/meta.txt"
        sudo "$REPO/scripts/spray.sh" show > "$d/spray_counters.txt" 2>&1
        # Reset counters between variants so each run's split can be checked.
        sudo "$REPO/scripts/spray.sh" up "$q" "$DELAY_US" "$IPERF_PORT" >/dev/null
    done
done
spray_down
bn_down
log "E4 done"
