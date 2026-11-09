#!/usr/bin/env bash
# Runs the full first-round evaluation (about 30 minutes), then bundles the
# results for copying off the node.
set -euo pipefail
S="$(dirname "${BASH_SOURCE[0]}")"
"$S/session_info.sh"
"$S/run_e0_noise.sh"
"$S/run_e1_compare.sh"
"$S/run_e2_target.sh"
"$S/run_e3_incast.sh"
"$S/run_e4_spray.sh"
OUT_ROOT="${OUT_ROOT:-/mydata/gswift}"
tar -C "$OUT_ROOT" -czf "$OUT_ROOT/results-$(date +%Y-%m-%d).tgz" "$(date +%Y-%m-%d)"
echo "Bundle: $OUT_ROOT/results-$(date +%Y-%m-%d).tgz"
