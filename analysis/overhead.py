"""E9: pacing fidelity and per-algorithm cost.

    python3 -m analysis.overhead results/2026-11-20

Pacing (e9_pacing/<mode>-c<X>): the window is pinned at X packets, so the
module intends X * MSS per smoothed RTT. Reports delivered goodput against
that intent and against the pacing rate the module actually set.

Cost (e9_cost/<name>): sender and receiver cores busy (from /proc/stat
deltas), cores per Gb/s, and kernel BPF time per invocation for each Swift
program (kernel.bpf_stats_enabled).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from analysis.loaders import load_iperf, load_meta as read_meta, load_trace

MSS = 1448  # payload bytes per full segment at MTU 1500 with timestamps


def pacing_row(run: Path) -> dict:
    meta = read_meta(run / "meta.txt")
    ip = load_iperf(run / "iperf.json")
    tr = load_trace(run / "trace.csv")
    row = {"run": run.name, "mode": meta.get("pace_mode"),
           "cwnd": float(meta.get("pin_cwnd", "nan")),
           "goodput_mbps": ip.goodput_bps / 1e6 if ip else np.nan}
    if tr is not None and len(tr):
        a = tr[(tr["type"] == 0) & (tr["srtt_us"] > 0)].sort_values("ts_ns")
        dt = np.diff(a["ts_ns"].to_numpy(), append=a["ts_ns"].iloc[-1]) if len(a) else []
        if len(a) and dt.sum() > 0:
            w = dt / dt.sum()
            intent = (a["cwnd"] * MSS * 8 / a["srtt_us"]).to_numpy()  # Mb/s
            paced = a["pacing_Bps"].to_numpy()
            set_rate = np.where(paced < 1e15, paced * 8 / 1e6, np.nan)
            row["srtt_us"] = float(np.sum(w * a["srtt_us"]))
            row["intent_mbps"] = float(np.sum(w * intent))
            row["set_mbps"] = float(np.nansum(w * set_rate) / np.sum(w[~np.isnan(set_rate)])) \
                if np.any(~np.isnan(set_rate)) else np.nan
    row["delivered_over_intent"] = row["goodput_mbps"] / row.get("intent_mbps", np.nan)
    row["delivered_over_set"] = row["goodput_mbps"] / row.get("set_mbps", np.nan)
    return row


def _cores(path: Path, ncpu: float) -> float:
    try:
        a, b = [list(map(int, ln.split()[1:])) for ln in path.read_text().splitlines()[:2]]
    except (OSError, ValueError):
        return np.nan
    d = np.array(b) - np.array(a)
    idle = d[3] + (d[4] if len(d) > 4 else 0)
    total = d.sum()
    return float((total - idle) / total * ncpu) if total else np.nan


def _bpf_progs(path: Path) -> dict[str, tuple[int, int]]:
    try:
        progs = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    return {p["name"]: (p.get("run_time_ns", 0), p.get("run_cnt", 0))
            for p in progs if p.get("name", "").startswith("swift")}


def cost_row(run: Path) -> dict:
    meta = read_meta(run / "meta.txt")
    ip = load_iperf(run / "iperf.json")
    gbps = ip.goodput_bps / 1e9 if ip else np.nan
    row = {"run": run.name, "goodput_gbps": gbps,
           "sender_cores": _cores(run / "cpu_sender.txt", float(meta.get("ncpu_sender", 1))),
           "receiver_cores": _cores(run / "cpu_receiver.txt", float(meta.get("ncpu_receiver", 1)))}
    row["sender_cores_per_gbps"] = row["sender_cores"] / gbps if gbps else np.nan
    before, after = _bpf_progs(run / "bpf_before.json"), _bpf_progs(run / "bpf_after.json")
    for name, (t1, c1) in after.items():
        t0, c0 = before.get(name, (0, 0))
        if c1 > c0:
            short = name.replace("swift_", "")
            row[f"ns_per_{short}"] = (t1 - t0) / (c1 - c0)
            row[f"calls_{short}"] = c1 - c0
    return row


def main(argv: list[str]) -> int:
    root = Path(argv[0])
    out = root / "tables"
    out.mkdir(exist_ok=True)
    for exp, fn in (("e9_pacing", pacing_row), ("e9_cost", cost_row)):
        runs = sorted(p for p in (root / exp).glob("*") if p.is_dir())
        if not runs:
            continue
        df = pd.DataFrame([fn(r) for r in runs])
        df.to_csv(out / f"{exp}.csv", index=False)
        print(exp)
        print(df.round(3).to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
