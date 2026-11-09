#!/usr/bin/env bash
# Records what CLAUDE.md requires for every session: node type, kernel,
# interface and NIC, whether tuning is applied, and the unloaded RTT baseline.
# Run on node0 after setup_node.sh on both nodes.
#
#   ./scripts/session_info.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
check_env
OUT="$OUT_ROOT/$DATE/session"
mkdir -p "$OUT"

describe() {  # runs locally or on the peer
    echo "hostname=$(hostname -f)"
    echo "node_type=$(cat /var/emulab/boot/nodetype 2>/dev/null || echo unknown)"
    echo "kernel=$(uname -r)"
    echo "os=$(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "cpu=$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2; exit}')"
    echo "iface=$1"
    echo "nic=$(ethtool -i "$1" | awk '/^driver|^firmware/ {printf "%s ", $0}')"
    echo "pci=$(lspci -s "$(ethtool -i "$1" | awk '/bus-info/ {print $2}')" 2>/dev/null | cut -d' ' -f2-)"
    echo "speed=$(cat /sys/class/net/"$1"/speed 2>/dev/null)Mb/s mtu=$(cat /sys/class/net/"$1"/mtu)"
    echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    echo "coalesce=$(sudo ethtool -c "$1" | awk '/^Adaptive|^rx-usecs:/ {printf "%s ", $0}')"
    echo "hwts_rx_all=$(ethtool -T "$1" | grep -qw all && echo yes || echo no)"
    echo "cc_available=$(sysctl -n net.ipv4.tcp_available_congestion_control)"
    echo "libbpf=$(pkg-config --modversion libbpf 2>/dev/null)"
}

{
    echo "# sender"
    describe "$IFACE"
    echo
    echo "# receiver"
    peer "$(declare -f describe); describe \$(ip -br addr | awk '\$3 ~ /^10\.10\.1\./ {print \$1; exit}')"
} | tee "$OUT/nodes.txt"

log "unloaded RTT baselines (no bottleneck)"
bn_down
sudo ping -c 2000 -i 0.001 -q "$PEER_IP" > "$OUT/ping_1ms.txt" 2>&1 || true
probe_server_start
sleep 0.5
peer "$AGG -s $SELF_IP:$PROBE_PORT -n 1 -b 64 -r 20000 -w 100 -o /tmp/gswift_rr.csv" 2>"$OUT/rr.log"
scp -q -o BatchMode=yes "$PEER:/tmp/gswift_rr.csv" "$OUT/rr_64B.csv"
probe_server_stop
python3 - "$OUT/rr_64B.csv" <<'EOF' | tee "$OUT/baseline.txt"
import sys, numpy as np, pandas as pd
x = pd.read_csv(sys.argv[1])["fct_us"].to_numpy()
p = np.percentile(x, [1, 50, 99, 99.9])
print(f"tcp_rr_64B_us n={len(x)} p1={p[0]:.1f} p50={p[1]:.1f} p99={p[2]:.1f} p99.9={p[3]:.1f}")
EOF
tail -2 "$OUT/ping_1ms.txt"
log "session info in $OUT"
