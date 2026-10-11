#!/usr/bin/env bash
# Apply low-latency tuning to a CloudLab node.
#
# These settings do NOT survive a reboot or a new experiment allocation.
# Run this on EVERY node at the start of EVERY session, or RTT measurements
# will not be comparable across runs.
#
# Measured effect on 2x r6525 (Clemson), 100G DAC, unloaded ICMP RTT:
#   untuned:  min 116 us  avg 395 us
#   tuned:    min  47 us  avg 119 us
#
# Usage:  ./tune.sh [interface]
#         If no interface is given, the one holding a 10.10.1.x address is used.

set -euo pipefail

IFACE="${1:-}"

if [[ -z "$IFACE" ]]; then
    IFACE=$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')
fi

if [[ -z "$IFACE" ]]; then
    echo "ERROR: could not find an interface with a 10.10.1.x address." >&2
    echo "Run 'ip -br addr' and pass the interface explicitly:" >&2
    echo "  ./tune.sh enp129s0f0np0" >&2
    exit 1
fi

echo "=== Tuning $IFACE on $(hostname) ==="

# cpupower lives in a kernel-version-specific package
if ! command -v cpupower >/dev/null 2>&1; then
    echo "--- installing linux-tools"
    sudo apt-get update -qq
    sudo apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)"
fi

# 1. Keep cores at full clock. Default (schedutil) ramps up only after work
#    arrives, which adds latency to the first packet of any exchange.
echo "--- CPU governor -> performance"
sudo cpupower frequency-set -g performance >/dev/null

# 2. Keep cores out of deep sleep. C-state exit latency is the single largest
#    contributor to the untuned RTT.
echo "--- deep C-states -> disabled"
sudo cpupower idle-set -D 0 >/dev/null

# 3. Interrupt coalescing lets the NIC sit on a received packet before raising
#    an interrupt. Default is adaptive with rx-usecs 8.
echo "--- interrupt coalescing -> off"
sudo ethtool -C "$IFACE" adaptive-rx off adaptive-tx off rx-usecs 0 tx-usecs 0

# 4. Load the congestion control modules we compare against. They ship as
#    loadable modules, so they do not appear in tcp_available_congestion_control
#    until loaded.
echo "--- loading congestion control modules"
sudo modprobe tcp_dctcp tcp_bbr tcp_vegas 2>/dev/null || true

echo
echo "=== Verification ==="
echo -n "governor:   "; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo -n "coalescing: "; sudo ethtool -c "$IFACE" | grep -i "^Adaptive"
echo -n "cc avail:   "; sysctl -n net.ipv4.tcp_available_congestion_control
echo
echo "Done. Interface: $IFACE"
