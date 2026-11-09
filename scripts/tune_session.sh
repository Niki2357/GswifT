#!/usr/bin/env bash
# Per-boot settings (subset of setup_node.sh): tuning, pacing qdisc, sysctls,
# quick ACKs. Re-run after any reboot.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sed -n '/^echo "=== tuning"/,$p' "$REPO/scripts/setup_node.sh" > /tmp/gswift_tail.sh
IFACE="${IFACE:-$(ip -br addr | awk '$3 ~ /^10\.10\.1\./ {print $1; exit}')}" REPO="$REPO" \
    bash -c "set -euo pipefail; $(cat /tmp/gswift_tail.sh)"
