"""Tests for analysis loaders and metrics, on small synthetic files."""

import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from analysis import loaders, metrics


# ---------- metrics ----------

def test_jain_equal_and_unequal() -> None:
    assert metrics.jain([5, 5, 5, 5]) == pytest.approx(1.0)
    assert metrics.jain([1, 0, 0, 0]) == pytest.approx(0.25)
    assert np.isnan(metrics.jain([]))


def test_parse_rate() -> None:
    assert metrics.parse_rate_bps("10gbit") == 10e9
    assert metrics.parse_rate_bps("500mbit") == 500e6
    assert metrics.parse_rate_bps("2.5Gbit") == 2.5e9
    with pytest.raises(ValueError):
        metrics.parse_rate_bps("fast")


def test_queue_delay_us() -> None:
    # 125000 bytes at 10 Gb/s = 100 us
    assert metrics.queue_delay_us(125000, 10e9) == pytest.approx(100.0)


def test_pct_summary_keys_and_values() -> None:
    s = metrics.pct_summary(np.arange(1, 101), "x")
    assert s["x_p50"] == pytest.approx(50.5)
    assert set(s) == {"x_p50", "x_p99", "x_p999", "x_mean"}


def test_pct_summary_empty_is_nan() -> None:
    s = metrics.pct_summary(np.array([]), "x")
    assert all(np.isnan(v) for v in s.values())


def test_round_completion() -> None:
    df = pd.DataFrame({"round": [0, 0, 1, 1], "flow": [0, 1, 0, 1],
                       "fct_us": [10.0, 30.0, 20.0, 5.0]})
    q = metrics.round_completion_us(df)
    assert list(q) == [30.0, 20.0]


# ---------- loaders ----------

def test_load_kv_handles_space_separated_first_line(tmp_path: Path) -> None:
    p = tmp_path / "s.txt"
    p.write_text("base_target_us=100 ai=1.000\nn_acks=42\nn_rto=0\n")
    kv = loaders.load_kv(p)
    assert kv["base_target_us"] == "100"
    assert kv["n_acks"] == "42"


def test_load_nstat(tmp_path: Path) -> None:
    p = tmp_path / "nstat.txt"
    p.write_text("#kernel\nTcpOutSegs                      1000               0.0\n"
                 "TcpRetransSegs                  10                 0.0\n")
    n = loaders.load_nstat(p)
    assert n["TcpOutSegs"] == 1000
    assert n["TcpRetransSegs"] == 10
    assert loaders.load_nstat(tmp_path / "missing.txt") == {}


def test_load_iperf(tmp_path: Path) -> None:
    doc = {"end": {
        "sum_received": {"bits_per_second": 9.5e9},
        "sum_sent": {"retransmits": 7},
        "streams": [{"receiver": {"bits_per_second": 4.0e9}},
                    {"receiver": {"bits_per_second": 5.5e9}}],
    }}
    p = tmp_path / "iperf.json"
    p.write_text(json.dumps(doc))
    r = loaders.load_iperf(p)
    assert r.goodput_bps == 9.5e9
    assert r.retransmits == 7
    assert r.per_flow_bps == (4.0e9, 5.5e9)


def test_load_iperf_error_document(tmp_path: Path) -> None:
    p = tmp_path / "iperf.json"
    p.write_text(json.dumps({"error": "unable to connect"}))
    assert loaders.load_iperf(p) is None


def test_load_trace_drops_partial_last_line(tmp_path: Path) -> None:
    p = tmp_path / "trace.csv"
    p.write_text("ts_ns,sport,dport,type,action,rtt_us,srtt_us,cwnd,snd_cwnd,"
                 "target_us,delay_us,pacing_Bps,inflight,acked,retx\n"
                 "1000,5201,40000,0,1,50,55,10.0,10,100,50,0,9,1,0\n"
                 "2000,5201,40000,1,2,250,90,6.0,6,100,250,0,6,1,0\n"
                 "3000,52")
    t = loaders.load_trace(p)
    assert len(t) == 2
    assert t["cwnd"].iloc[1] == 6.0


def test_load_run_collects_meta(tmp_path: Path) -> None:
    d = tmp_path / "swift-n1"
    d.mkdir()
    (d / "meta.txt").write_text("variant=swift\nflows=1\nbn_rate=10gbit\n")
    run = loaders.load_run(d)
    assert run.meta["variant"] == "swift"
    assert run.name == "swift-n1"
    assert run.iperf is None and run.trace is None


def _run_with(tmp_path: Path, name: str, date: str, out: float,
              extra: str = "") -> loaders.Run:
    d = tmp_path / name
    d.mkdir()
    (d / "meta.txt").write_text(f"date={date}\n{extra}")
    (d / "nstat.txt").write_text(f"TcpOutSegs {out} 0\nTcpRetransSegs {out / 100} 0\n")
    return loaders.load_run(d)


def test_cumulative_to_delta(tmp_path: Path) -> None:
    a = _run_with(tmp_path, "a", "2026-11-09T12:00", 1000)
    b = _run_with(tmp_path, "b", "2026-11-09T12:01", 3000)    # absolute
    c = _run_with(tmp_path, "c", "2026-11-09T12:02", 500)     # already a delta
    fa, fb, fc = loaders.cumulative_to_delta([a, b, c])
    assert fa.nstat == {}
    assert fb.nstat["TcpOutSegs"] == 2000
    assert fb.nstat["TcpRetransSegs"] == 20
    assert fc.nstat["TcpOutSegs"] == 500


def test_cumulative_to_delta_skips_runs_marked_delta(tmp_path: Path) -> None:
    a = _run_with(tmp_path, "a", "2026-11-10T12:00", 1000, "nstat_mode=delta\n")
    b = _run_with(tmp_path, "b", "2026-11-10T12:01", 3000, "nstat_mode=delta\n")
    fa, fb = loaders.cumulative_to_delta([a, b])
    assert fa.nstat["TcpOutSegs"] == 1000
    assert fb.nstat["TcpOutSegs"] == 3000
