#!/usr/bin/env bash
# Single-run building blocks used by the run_e*.sh experiment scripts.
# Source after common.sh.

# Default Swift parameters for every Swift variant; experiments append to it.
SWIFT_BASE_ARGS="${SWIFT_BASE_ARGS:---base-target 100 --trace-every 4}"

# Maps a variant label to "<kernel cc>|<extra swiftctl args>".
variant() {
    case "$1" in
        swift)        echo "swift|" ;;
        swift-nofrac) echo "swift|--min-cwnd 1" ;;
        swift-fs)     echo "swift|--fs-range 200" ;;
        mswift)       echo "swift|--median-cap 16" ;;
        mswift-k4)    echo "swift|--median-cap 16 --reorder-k 4" ;;
        cubic|dctcp|vegas|bbr|reno) echo "$1|" ;;
        *) die "unknown variant '$1'" ;;
    esac
}

# Loads Swift if the variant needs it. Sets KCC (kernel cc name).
begin_variant() {  # begin_variant <dir> <label> [extra swift args...]
    local dir="$1" label="$2"; shift 2
    local v; v="$(variant "$label")"
    KCC="${v%%|*}"
    local vargs="${v#*|}"
    if [[ "$KCC" == swift ]]; then
        # shellcheck disable=SC2086
        start_swift "$dir" $SWIFT_BASE_ARGS $vargs "$@"
    fi
    write_meta "$dir" "variant=$label" "kcc=$KCC" \
        "swift_args=$SWIFT_BASE_ARGS $vargs $*"
}

# Bulk transfer through the bottleneck with a concurrent latency probe.
run_bulk() {  # run_bulk <dir> <label> <flows> <seconds> [extra swift args...]
    local dir="$1" label="$2" flows="$3" dur="$4"; shift 4
    log "bulk: $label flows=$flows ${dur}s -> $dir"
    begin_variant "$dir" "$label" "$@"
    echo "flows=$flows" >> "$dir/meta.txt"
    echo "duration_s=$dur" >> "$dir/meta.txt"
    ensure_iperf_server
    probe_server_start
    nstat_reset
    qmon_start
    probe_run "$dir" "$((dur + 1))"
    sleep 0.5
    iperf3 -c "$PEER_IP" -p "$IPERF_PORT" -C "$KCC" -P "$flows" -t "$dur" \
        -J ${IPERF_EXTRA:-} > "$dir/iperf.json" || log "iperf3 failed (see iperf.json)"
    probe_collect "$dir"
    qmon_stop "$dir"
    nstat_save "$dir"
    probe_server_stop
    stop_swift
    sleep 1
}

# Synchronized incast rounds: N flows each send <bytes> to the receiver.
run_incast() {  # run_incast <dir> <label> <flows> <bytes> <rounds> [extra swift args...]
    local dir="$1" label="$2" flows="$3" bytes="$4" rounds="$5"; shift 5
    log "incast: $label N=$flows ${bytes}B x $rounds rounds -> $dir"
    begin_variant "$dir" "$label" "$@"
    printf 'flows=%s\nbytes=%s\nrounds=%s\n' "$flows" "$bytes" "$rounds" >> "$dir/meta.txt"
    "$WORKER" -p "$INCAST_PORT" -C "$KCC" 2>"$dir/worker.log" &
    local wpid=$!
    sleep 0.5
    nstat_reset
    qmon_start
    peer "$AGG -s $SELF_IP:$INCAST_PORT -n $flows -b $bytes -r $rounds -g 2000 -w 5 \
          -o /tmp/gswift_incast.csv" 2>"$dir/agg.log" || log "incast_agg failed (see agg.log)"
    scp -q -o BatchMode=yes "$PEER:/tmp/gswift_incast.csv" "$dir/incast.csv" || log "no incast data"
    qmon_stop "$dir"
    nstat_save "$dir"
    kill "$wpid" 2>/dev/null || true
    wait "$wpid" 2>/dev/null || true
    stop_swift
    sleep 1
}
