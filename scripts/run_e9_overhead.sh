#!/usr/bin/env bash
# E9: implementation costs and fidelity.
#
# Part A, pacing fidelity: one flow, no bottleneck, Swift's window pinned at
# X packets (min = max = init). Compares the delivered rate with the rate the
# module requested, without and with the optional 1/2 sub-packet correction
# (--pace-halve), and with a two-packet Linux window (--min-snd-cwnd 2).
#
# Part B, cost: 8 flows through the bottleneck. Per algorithm, sender and
# receiver CPU (from /proc/stat) and, for Swift, kernel BPF run time per
# program invocation (kernel.bpf_stats_enabled).
#
#   ./scripts/run_e9_overhead.sh                   (about 12 minutes)
#   PARTS=A CWNDS="0.1 0.5" ./scripts/run_e9_overhead.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap 'sudo sysctl -qw kernel.bpf_stats_enabled=0; cleanup_all' EXIT
PARTS="${PARTS:-A B}"
CWNDS="${CWNDS:-0.05 0.1 0.25 0.5 0.9}"
DUR="${DUR:-8}"
BPFTOOL="${BPFTOOL:-$(command -v bpftool || ls /usr/lib/linux-tools/*/bpftool 2>/dev/null | head -1)}"

cpu_snap() { head -1 /proc/stat; }   # aggregate "cpu" line
peer_cpu_snap() { peer head -1 /proc/stat; }

if [[ " $PARTS " == *" A "* ]]; then
    bn_down
    for REP in $(rep_list); do
    for mode in ${PACE_MODES:-raw halve pair}; do
        for x in $CWNDS; do
            case "$mode" in
                raw)   extra="" ;;
                halve) extra="--pace-halve" ;;
                pair)  extra="--min-snd-cwnd 2" ;;
            esac
            d="$(run_dir e9_pacing "$mode-c$x$(rep_suffix "$REP")")"
            SWIFT_BASE_ARGS="--trace-every 1" run_bulk "$d" swift 1 "$DUR" \
                --min-cwnd "$x" --max-cwnd "$x" --init-cwnd "$x" $extra
            printf 'pin_cwnd=%s\npace_mode=%s\n' "$x" "$mode" >> "$d/meta.txt"
        done
    done
    done
fi

if [[ " $PARTS " == *" B "* ]]; then
    [[ -n "$BPFTOOL" ]] || die "bpftool not found"
    sudo sysctl -qw kernel.bpf_stats_enabled=1
    bn_up
    for label in cubic dctcp swift:--trace-every=0 swift:--trace-every=16 \
                 swift:--trace-every=1 mswift:--trace-every=0; do
        v="${label%%:*}"; targs=""
        [[ "$label" == *:* ]] && targs="${label#*:}" && targs="${targs/=/ }"
        [[ "$targs" == "--trace-every 0" ]] && targs="$targs --no-trace-events"
        name="$v"; [[ "$label" == *:* ]] && name="$v-te${label##*=}"
        d="$(run_dir e9_cost "$name")"
        log "cost: $name"
        # shellcheck disable=SC2086
        SWIFT_BASE_ARGS="--base-target 300" begin_variant "$d" "$v" $targs
        printf 'flows=8\nduration_s=%s\n' "$DUR" >> "$d/meta.txt"
        ensure_iperf_server 8
        nstat_reset
        [[ "$KCC" == swift ]] && sudo "$BPFTOOL" prog show -j > "$d/bpf_before.json"
        c0="$(cpu_snap)"; p0="$(peer_cpu_snap)"
        iperf_clients "$d" 8 "$DUR"
        c1="$(cpu_snap)"; p1="$(peer_cpu_snap)"
        [[ "$KCC" == swift ]] && sudo "$BPFTOOL" prog show -j > "$d/bpf_after.json"
        printf '%s\n%s\n' "$c0" "$c1" > "$d/cpu_sender.txt"
        printf '%s\n%s\n' "$p0" "$p1" > "$d/cpu_receiver.txt"
        echo "ncpu_sender=$(nproc)" >> "$d/meta.txt"
        echo "ncpu_receiver=$(peer nproc)" >> "$d/meta.txt"
        nstat_save "$d"
        stop_swift
        sleep 1
    done
    bn_down
    sudo sysctl -qw kernel.bpf_stats_enabled=0
fi
log "E9 done"
