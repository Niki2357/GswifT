#!/usr/bin/env bash
# One-time setup on EACH node, every session: install dependencies, build the
# module and tools, apply tuning, and set the sysctls all experiments assume.
#
#   cd GswifT && ./scripts/setup_node.sh

set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}"
[[ -n "$IFACE" ]] || { echo "no 10.10.1.x interface; pass IFACE=..." >&2; exit 1; }

echo "=== packages"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    clang llvm libbpf-dev libelf-dev zlib1g-dev pkg-config make gcc \
    "linux-tools-$(uname -r)" linux-tools-common iperf3 ethtool iproute2 \
    iptables python3 python3-numpy python3-pandas python3-matplotlib >/dev/null

echo "=== libbpf"
# Ubuntu 22.04 ships libbpf 0.5, too old for skeletons from a 6.x bpftool.
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
if ! pkg-config --atleast-version=1.0 libbpf; then
    rm -rf /tmp/libbpf
    git clone -q --depth 1 -b v1.4.5 https://github.com/libbpf/libbpf /tmp/libbpf
    sudo make -s -C /tmp/libbpf/src install PREFIX=/usr/local LIBDIR=/usr/local/lib >/dev/null
    sudo ldconfig
fi
echo "libbpf $(pkg-config --modversion libbpf)"

echo "=== build"
make -C "$REPO/src" info
make -C "$REPO/src"
make -C "$REPO/src" check || echo "WARNING: bpftool verifier check failed; smoke_test.sh will confirm loading"
make -C "$REPO/tools/incast"
make -C "$REPO/tests" test

echo "=== tuning"
bash "$REPO/verification/scripts/tune.sh" "$IFACE"

echo "=== pacing qdisc"
# Before 6.0 a BPF congestion control cannot set pacing itself, so fq enforces
# sk_pacing_rate (applied for every algorithm, so all runs share one path).
# On 6.0+ the module enables TCP's internal pacing and the default stays.
if [[ "$(printf '%s\n6.0\n' "$(uname -r | cut -d- -f1)" | sort -V | head -1)" != 6.0 ]]; then
    sudo sysctl -qw net.core.default_qdisc=fq
    sudo tc qdisc replace dev "$IFACE" root mq 2>/dev/null || sudo tc qdisc replace dev "$IFACE" root fq
fi
tc qdisc show dev "$IFACE" | head -2 || true

echo "=== sysctls"
# Keep cwnd across idle periods between incast rounds (all algorithms).
sudo sysctl -qw net.ipv4.tcp_slow_start_after_idle=0
# Let DCTCP negotiate ECN when the peer asks.
sudo sysctl -qw net.ipv4.tcp_ecn=1
# Large socket buffers so window, not buffer, limits throughput.
sudo sysctl -qw net.core.rmem_max=268435456 net.core.wmem_max=268435456
sudo sysctl -qw net.ipv4.tcp_rmem="4096 131072 268435456"
sudo sysctl -qw net.ipv4.tcp_wmem="4096 131072 268435456"
# Delayed ACKs inflate RTT samples and stall sub-1 windows (see report 1,
# section 4.5). Quick ACKs on every route of the experiment subnet.
ROUTE="$(ip route show dev "$IFACE" | grep -m1 '^10\.10\.1\.' | sed 's/ quickack 1//')"
if [[ -n "$ROUTE" ]]; then
    # shellcheck disable=SC2086
    sudo ip route replace $ROUTE dev "$IFACE" quickack 1
fi
ip route show dev "$IFACE"

echo "=== done on $(hostname)"
