"""End-to-end: build a synthetic results tree in the formats the run scripts
write, then run the analysis CLI over it. Catches format mismatches before a
CloudLab session. The numbers here are fake and only exercise the code."""

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from analysis import analyze

RNG = np.random.default_rng(0)
TRACE_COLS = ["ts_ns", "sport", "dport", "type", "action", "rtt_us", "srtt_us", "cwnd",
              "snd_cwnd", "target_us", "delay_us", "pacing_Bps", "inflight", "acked", "retx"]


def _meta(d: Path, **kv: object) -> None:
    base = {"bn_rate": "10gbit", "bn_limit": 300, "duration_s": 5}
    base.update(kv)
    (d / "meta.txt").write_text("".join(f"{k}={v}\n" for k, v in base.items()))


def _trace(d: Path, n: int = 500) -> None:
    rtt = RNG.integers(40, 200, n)
    df = pd.DataFrame({
        "ts_ns": np.arange(n) * 10_000, "sport": 40000, "dport": 5201 + (np.arange(n) % 2),
        "type": np.where(np.arange(n) % 50 == 0, 1, 0), "action": 1, "rtt_us": rtt,
        "srtt_us": 80, "cwnd": RNG.uniform(0.5, 20, n), "snd_cwnd": 5, "target_us": 100,
        "delay_us": rtt, "pacing_Bps": 0, "inflight": 4, "acked": 1, "retx": 0})
    df[TRACE_COLS].to_csv(d / "trace.csv", index=False)
    (d / "swift_stats.txt").write_text("base_target_us=100 ai=1\nn_acks=500\nn_frac_acks=50\n")


def _bulk_files(d: Path, gbps: float) -> None:
    (d / "iperf.json").write_text(json.dumps({"end": {
        "sum_received": {"bits_per_second": gbps * 1e9}, "sum_sent": {"retransmits": 3},
        "streams": [{"receiver": {"bits_per_second": gbps * 5e8}}] * 2}}))
    ts = 1000 + np.arange(600) * 0.01
    pd.DataFrame({"ts": ts, "backlog_bytes": RNG.integers(0, 200000, 600),
                  "backlog_pkts": 10, "drops": np.arange(600) // 100, "ce_marks": 0,
                  "sent_bytes": 0}).to_csv(d / "qmon.csv", index=False)
    pd.DataFrame({"round": range(1000), "flow": 0,
                  "fct_us": RNG.uniform(30, 300, 1000)}).to_csv(d / "probe.csv", index=False)
    (d / "nstat.txt").write_text("#kernel\nTcpOutSegs 100000 0\nTcpRetransSegs 50 0\n"
                                 "TcpExtTCPSACKReorder 7 0\n")


def _run(root: Path, exp: str, name: str, swift: bool = True, **meta: object) -> Path:
    d = root / exp / name
    d.mkdir(parents=True)
    _meta(d, **meta)
    _bulk_files(d, gbps=9.0)
    if swift:
        _trace(d)
    return d


def test_full_pipeline(tmp_path: Path, monkeypatch) -> None:
    root = tmp_path / "2026-11-09"
    (root / "session").mkdir(parents=True)
    (root / "session" / "baseline.txt").write_text("tcp_rr_64B_us n=100 p1=30.0 p50=40.0\n")
    for load in ("1G", "max"):
        _run(root, "e0_noise", f"load-{load}", offered_load=load, variant="swift", flows=1)
    for v in ("swift", "cubic"):
        _run(root, "e1_compare", f"{v}-n8", swift=v == "swift", variant=v, flows=8)
    for t in (50, 100):
        _run(root, "e2_target", f"t{t}", variant="swift", flows=8, base_target_us=t)
    for v in ("swift", "cubic"):
        for n in (16, 64):
            d = _run(root, "e3_incast", f"{v}-n{n}", swift=v == "swift", variant=v, flows=n)
            pd.DataFrame({"round": np.repeat(range(20), n), "flow": np.tile(range(n), 20),
                          "fct_us": RNG.uniform(100, 5000, 20 * n)}).to_csv(
                d / "incast.csv", index=False)
    for v in ("swift", "mswift"):
        for q in (0, 0.1):
            d = _run(root, "e4_spray", f"{v}-q{q}", variant=v, flows=8, q=q)
            (d / "spray_counters.txt").write_text(
                "    pkts      bytes target\n    1000   9000000 MARK  tcp\n"
                "qdisc prio 1: root refcnt 2 bands 2\n Sent 90000000 bytes 10000 pkt\n")

    monkeypatch.setattr(sys, "argv", ["analyze", str(root)])
    analyze.main()

    summary = (root / "summary.md").read_text()
    for title in ("E0", "E1", "E2", "E3", "E4"):
        assert title in summary
    figs = {p.name for p in (root / "figs").glob("*.png")}
    assert {"e0_rtt_cdf.png", "e1_tradeoff.png", "e1_queue.png", "e2_target.png",
            "e3_incast.png", "e4_spray.png", "e1_trace_swift-n8.png"} <= figs
    e4 = pd.read_csv(root / "tables" / "e4.csv")
    assert np.allclose(e4["q_measured"], 0.1)
    e1 = pd.read_csv(root / "tables" / "e1.csv")
    assert e1.loc[e1.variant == "swift", "frac_ack_pct"].iloc[0] == 10.0


def test_repeats_are_collapsed(tmp_path: Path, monkeypatch) -> None:
    root = tmp_path / "2026-11-10"
    root.mkdir()
    for rep in (1, 2, 3):
        for v in ("swift", "cubic"):
            _run(root, "e1_compare", f"{v}-n8-r{rep}", swift=v == "swift", variant=v,
                 flows=8, rep=rep, nstat_mode="delta")
        for tol in ("default", "64"):
            d = _run(root, "e4_spray", f"swift-q0.1-{tol}-r{rep}", variant="swift", flows=8,
                     q=0.1, rep=rep, reorder_tol=tol, nstat_mode="delta")
            (d / "spray_counters.txt").write_text("")

    monkeypatch.setattr(sys, "argv", ["analyze", str(root)])
    analyze.main()

    summary = (root / "summary.md").read_text()
    assert "Median (min-max)" in summary
    agg = pd.read_csv(root / "tables" / "e1_agg.csv")
    assert set(agg["n"]) == {3}
    assert {"util_pct", "util_pct_lo", "util_pct_hi"} <= set(agg.columns)
    e4 = pd.read_csv(root / "tables" / "e4_agg.csv")
    assert len(e4) == 2 and set(e4["n"]) == {3}
    # Delta-mode nstat is kept as written, not differenced across runs.
    e1 = pd.read_csv(root / "tables" / "e1.csv")
    assert np.allclose(e1["retrans_pct"], 0.05)
    assert (root / "figs" / "e4_spray.png").exists()
