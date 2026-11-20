#!/usr/bin/env bash
# E8: realistic RPC workloads (the Homa/Linux-style benchmark). Poisson
# arrivals, response sizes from an empirical distribution, offered load as a
# fraction of the bottleneck. Reports latency per RPC; analysis computes
# slowdown versus message size.
#
#   ./scripts/run_e8_workload.sh                         (about 25 min at REPS=3)
#   WORKLOADS=websearch LOADS=0.8 VARIANTS="swift-t200 dctcp" ./scripts/run_e8_workload.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
[[ -x "$RPC_LOAD" ]] || die "rpc_load not built (make -C tools/incast)"
peer test -x "$RPC_LOAD" || die "rpc_load not built on $PEER"
trap cleanup_all EXIT
DUR="${DUR:-20}"
WORKLOADS="${WORKLOADS:-rpcmix websearch}"
LOADS="${LOADS:-0.5 0.8}"
VARIANTS="${VARIANTS:-swift swift-t200 cubic dctcp}"
TAG="${TAG:-}"

bn_up
for REP in $(rep_list); do
    for w in $WORKLOADS; do
        for l in $LOADS; do
            for v in $VARIANTS; do
                d="$(run_dir e8_workload "$w-l$l-$v$TAG$(rep_suffix "$REP")")"
                SWIFT_BASE_ARGS="--base-target 100 --trace-every 16" \
                    run_rpc "$d" "$v" "$REPO/tools/incast/workloads/$w.cdf" "$l" "$DUR"
            done
        done
    done
done
bn_down
log "E8 done"
