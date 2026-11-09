#!/usr/bin/env bash
# E1: Swift vs Cubic, DCTCP, Vegas, BBR through the same bottleneck.
# Throughput (iperf3), queuing (qdisc backlog), RPC latency under load (probe),
# retransmissions (nstat). Swift runs also record cwnd/RTT/target traces.
#
#   ./scripts/run_e1_compare.sh          (about 6 minutes)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap cleanup_all EXIT
DUR="${DUR:-20}"
VARIANTS="${VARIANTS:-swift cubic dctcp vegas bbr}"
FLOWS="${FLOWS:-1 8}"

bn_up
for n in $FLOWS; do
    for v in $VARIANTS; do
        run_bulk "$(run_dir e1_compare "$v-n$n")" "$v" "$n" "$DUR"
    done
done
bn_down
log "E1 done"
