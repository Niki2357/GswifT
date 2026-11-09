#!/usr/bin/env bash
# Quick end-to-end check (about 2 minutes) before the real runs. Each step
# prints PASS/FAIL; paste the output if anything fails.
#
#   ./scripts/smoke_test.sh

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_runs.sh"
check_env
trap 'sudo "$REPO/scripts/spray.sh" down >/dev/null 2>&1; cleanup_all' EXIT
OUT="$OUT_ROOT/$DATE/smoke"
mkdir -p "$OUT"
FAILS=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); }
gbps() { python3 -c "import json,sys; print(f\"{json.load(open(sys.argv[1]))['end']['sum_received']['bits_per_second']/1e9:.3f}\")" "$1" 2>/dev/null || echo 0; }
ensure_iperf_server

# 1. Module registers and carries traffic, with RTT samples and traces.
start_swift "$OUT" --base-target 100 --trace-every 1
iperf3 -c "$PEER_IP" -C swift -t 5 -J > "$OUT/swift.json" || true
stop_swift
acks=$(awk -F= '/^n_acks=/ {print $2}' "$OUT/swift_stats.txt" 2>/dev/null || echo 0)
samples=$(awk -F= '/^n_samples=/ {print $2}' "$OUT/swift_stats.txt" 2>/dev/null || echo 0)
lines=$(wc -l < "$OUT/trace.csv" 2>/dev/null || echo 0)
rate=$(gbps "$OUT/swift.json")
[[ "${acks:-0}" -gt 1000 && "${samples:-0}" -gt 100 && "$lines" -gt 100 ]] &&
    pass "swift loads: $rate Gb/s, acks=$acks samples=$samples trace_lines=$lines" ||
    fail "swift loads: rate=$rate acks=$acks samples=$samples trace_lines=$lines (see $OUT/swiftctl.log)"

# 2. Fractional window: cwnd pinned at 0.5 must give ~ mss * 0.5 / srtt.
start_swift "$OUT" --init-cwnd 0.5 --min-cwnd 0.5 --max-cwnd 1 --ai 0.001 \
    --base-target 1 --trace-every 1
iperf3 -c "$PEER_IP" -C swift -t 5 -J > "$OUT/frac.json" || true
stop_swift
python3 - "$OUT/trace.csv" "$OUT/frac.json" <<'EOF' && pass "fractional cwnd paces as expected" || fail "fractional cwnd"
import json, sys, pandas as pd
t = pd.read_csv(sys.argv[1]); t = t[t.type == 0]
got = json.load(open(sys.argv[2]))["end"]["sum_received"]["bits_per_second"]
srtt = t.srtt_us.median()
want = (t.pacing_Bps.median()) * 8
print(f"   cwnd median {t.cwnd.median():.3f}, snd_cwnd max {t.snd_cwnd.max()}, srtt {srtt:.0f} us")
print(f"   pacing setting {want/1e6:.1f} Mb/s, measured {got/1e6:.1f} Mb/s")
ok = t.cwnd.max() < 1 and t.snd_cwnd.max() == 1 and 0.6 < got / want < 1.15
sys.exit(0 if ok else 1)
EOF

# 3. Bottleneck limits rate and builds a queue.
bn_up
iperf3 -c "$PEER_IP" -C cubic -P 4 -t 5 -J > "$OUT/bn.json" || true
peer sudo tc -s qdisc show dev ifb0 > "$OUT/bn_tc.txt"
rate=$(gbps "$OUT/bn.json")
python3 -c "import sys; r=float(sys.argv[1]); sys.exit(0 if 0.85*${BN_RATE%gbit} < r < 1.05*${BN_RATE%gbit} else 1)" "$rate" &&
    pass "bottleneck: cubic x4 = $rate Gb/s (limit $BN_RATE)" ||
    fail "bottleneck: cubic x4 = $rate Gb/s, expected about $BN_RATE"

# 4. Queue monitor and probe produce data.
d="$(run_dir smoke run_bulk)"
run_bulk "$d" swift 2 5
[[ -s "$d/qmon.csv" && -s "$d/probe.csv" && -s "$d/iperf.json" ]] &&
    pass "run_bulk outputs: $(wc -l < "$d/qmon.csv") qmon rows, $(wc -l < "$d/probe.csv") probe rows" ||
    fail "run_bulk outputs missing in $d"

# 5. Incast tool.
d="$(run_dir smoke run_incast)"
run_incast "$d" swift 16 65536 20
[[ -s "$d/incast.csv" ]] && pass "incast: $(tail -1 "$d/agg.log")" || fail "incast (see $d/agg.log)"
bn_down

# 6. Spraying marks about q of packets.
sudo "$REPO/scripts/spray.sh" up 0.2 300 "$IPERF_PORT" >/dev/null
iperf3 -c "$PEER_IP" -C cubic -t 3 > /dev/null || true
sudo "$REPO/scripts/spray.sh" show > "$OUT/spray.txt"
sudo "$REPO/scripts/spray.sh" down >/dev/null
python3 - "$OUT/spray.txt" <<'EOF' && pass "spraying split" || fail "spraying split (see $OUT/spray.txt)"
import re, sys
txt = open(sys.argv[1]).read()
marked = int(re.search(r"^\s*(\d+)\s+\d+\s+MARK", txt, re.M).group(1))
sent = {m.group(1): int(m.group(2)) for m in re.finditer(r"qdisc \S+ (\d+): .*?\n Sent \d+ bytes (\d+) pkt", txt, re.S)}
total = sent.get("1", 0)
print(f"   marked {marked} of {total} packets = {marked/max(total,1):.3f} (want ~0.2)")
sys.exit(0 if total and 0.15 < marked / total < 0.25 else 1)
EOF

echo
[[ $FAILS -eq 0 ]] && echo "ALL PASS" || echo "$FAILS FAILED (outputs in $OUT)"
