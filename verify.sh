#!/usr/bin/env bash
# Capture the environment state of a CloudLab node into /mydata/verify.
#
# Run this on every node at the start of a session, after tune.sh, so each set
# of results carries a record of the machine it came from.
#
# Usage:  ./verify.sh [interface]

set -uo pipefail

IFACE="${1:-}"
if [[ -z "$IFACE" ]]; then
    IFACE=$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')
fi

if [[ -z "$IFACE" ]]; then
    echo "ERROR: no 10.10.1.x interface found. Pass one explicitly." >&2
    exit 1
fi

OUT=/mydata/verify/$(hostname)
sudo mkdir -p "$OUT"
sudo chown "$USER" "$OUT"
cd "$OUT"

echo "=== Capturing to $OUT (interface $IFACE) ==="

hostname                                            > hostname.txt
uname -r                                            > kernel.txt
lspci | grep -i mellanox                            > nics.txt
ip -br addr                                         > interfaces.txt
ls /dev/ptp*                                        > ptp.txt
sudo ethtool -T "$IFACE"                            > ethtool-T.txt
sudo ethtool "$IFACE"                               > ethtool.txt
sudo ethtool -c "$IFACE"                            > coalescing.txt
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor > governor.txt
sysctl net.ipv4.tcp_available_congestion_control    > cc-available.txt
ls -l /sys/kernel/btf/vmlinux                       > btf.txt
grep -E "CONFIG_BPF_SYSCALL|CONFIG_BPF_JIT|CONFIG_DEBUG_INFO_BTF" \
     "/boot/config-$(uname -r)"                     > bpf-config.txt
sudo bpftool feature probe 2>/dev/null | grep -i struct_ops > struct-ops.txt

echo
echo "--- hardware timestamping (the one that matters) ---"
grep -A3 "Hardware Receive Filter Modes" ethtool-T.txt || cat ethtool-T.txt

if grep -q "^[[:space:]]*all$" ethtool-T.txt; then
    echo
    echo "OK: HWTSTAMP_FILTER_ALL supported on $IFACE"
else
    echo
    echo "WARNING: HWTSTAMP_FILTER_ALL not found on $IFACE."
    echo "Check the other Mellanox interface, or this node will not work"
    echo "for per-packet receive timestamping."
fi

echo
echo "Files written to $OUT"
ls -1 "$OUT"
