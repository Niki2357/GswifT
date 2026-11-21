#!/usr/bin/env bash
# Source before an experiment script to use the veth/netns bottleneck:
#   source scripts/use_veth.sh && ./scripts/run_e8_workload.sh
# Sets up the receiver netns on node1 and the route on node0.
_r="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export BN_KIND=veth RX_NS=rx PEER_IP=10.10.2.2 QMON_DEV=vh
ssh -o BatchMode=yes node1 "sudo $_r/scripts/rxns.sh up" >/dev/null
_if="$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')"
sudo ip route replace 10.10.2.0/24 via 10.10.1.2 dev "$_if" quickack 1
echo "veth bottleneck selected (receiver 10.10.2.2 in netns rx)"
