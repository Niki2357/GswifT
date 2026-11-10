"""One summary table (DataFrame) per experiment, one row per run."""

import re

import numpy as np
import pandas as pd

from analysis import metrics
from analysis.loaders import Run

TRIM_S = 1.5        # ignore queue samples this close to the run's edges
EV_ACK = 0          # trace event types, see src/swift_types.h
EV_DECREASE = 1


def _f(meta: dict[str, str], key: str, default: float = float("nan")) -> float:
    try:
        return float(meta[key])
    except (KeyError, ValueError):
        return default


def _rate_bps(run: Run) -> float:
    try:
        return metrics.parse_rate_bps(run.meta.get("bn_rate", ""))
    except ValueError:
        return float("nan")


def _busy_qmon(q: pd.DataFrame | None) -> pd.DataFrame | None:
    """Queue samples away from the start/end of the run, when traffic flowed."""
    if q is None or q.empty:
        return None
    t = q["ts"]
    mid = q[(t > t.min() + TRIM_S) & (t < t.max() - TRIM_S)]
    return mid if len(mid) else q


def queue_stats(run: Run) -> dict[str, float]:
    q = _busy_qmon(run.qmon)
    rate = _rate_bps(run)
    if q is None or np.isnan(rate):
        return {"queue_us_p50": np.nan, "queue_us_p99": np.nan, "drops": np.nan}
    d = metrics.queue_delay_us(q["backlog_bytes"].to_numpy(dtype=float), rate)
    p50, p99 = np.percentile(d, [50, 99])
    drops = run.qmon["drops"].iloc[-1] - run.qmon["drops"].iloc[0]
    return {"queue_us_p50": p50, "queue_us_p99": p99, "drops": float(drops)}


def probe_stats(run: Run) -> dict[str, float]:
    if run.probe is None:
        return metrics.pct_summary(np.array([]), "rpc_us")
    x = run.probe["fct_us"].to_numpy(dtype=float)
    k = len(x) // 20                       # drop 5% at each edge (idle time)
    return metrics.pct_summary(x[k:len(x) - k] if k else x, "rpc_us")


def swift_stats(run: Run) -> dict[str, float]:
    out = {"rtt_us_p50": np.nan, "rtt_us_p99": np.nan, "target_us": np.nan,
           "cwnd_p50": np.nan, "decr_per_s": np.nan, "frac_ack_pct": np.nan}
    t = run.trace
    if t is not None and len(t):
        acks = t[(t["type"] == EV_ACK) & (t["rtt_us"] > 0)]
        if len(acks):
            out["rtt_us_p50"], out["rtt_us_p99"] = np.percentile(acks["rtt_us"], [50, 99])
            out["target_us"] = float(acks["target_us"].median())
            out["cwnd_p50"] = float(acks["cwnd"].median())
        span = (t["ts_ns"].max() - t["ts_ns"].min()) / 1e9
        if span > 0:
            out["decr_per_s"] = float((t["type"] == EV_DECREASE).sum() / span)
    acks_n = _f(run.stats, "n_acks", 0)
    if acks_n:
        out["frac_ack_pct"] = 100 * _f(run.stats, "n_frac_acks", 0) / acks_n
    return out


def bulk_row(run: Run) -> dict[str, float | str]:
    rate = _rate_bps(run)
    gp = run.iperf.goodput_bps if run.iperf else np.nan
    row: dict[str, float | str] = {
        "run": run.name,
        "rep": _f(run.meta, "rep", 1),
        "variant": run.meta.get("variant", "?"),
        "flows": _f(run.meta, "flows"),
        "goodput_gbps": gp / 1e9,
        "util_pct": 100 * gp / rate if rate == rate else np.nan,
        "jain": metrics.jain(run.iperf.per_flow_bps) if run.iperf else np.nan,
        "retrans_pct": metrics.retrans_pct(run.nstat),
    }
    row.update(queue_stats(run))
    row.update(probe_stats(run))
    row.update(swift_stats(run))
    return row


def summarize_e0(runs: list[Run]) -> pd.DataFrame:
    rows = []
    for r in runs:
        acks = r.trace[(r.trace["type"] == EV_ACK) & (r.trace["rtt_us"] > 0)] \
            if r.trace is not None else pd.DataFrame({"rtt_us": []})
        x = acks["rtt_us"].to_numpy(dtype=float)
        p = np.percentile(x, [1, 50, 99, 99.9]) if len(x) else [np.nan] * 4
        rows.append({
            "load": r.meta.get("offered_load", r.name),
            "goodput_gbps": r.iperf.goodput_bps / 1e9 if r.iperf else np.nan,
            "samples": len(x),
            "rtt_us_min": x.min() if len(x) else np.nan,
            "rtt_us_p1": p[0], "rtt_us_p50": p[1], "rtt_us_p99": p[2], "rtt_us_p999": p[3],
            "excess_us_p50": p[1] - p[0], "excess_us_p99": p[2] - p[0],
        })
    return pd.DataFrame(rows)


def summarize_bulk(runs: list[Run]) -> pd.DataFrame:
    df = pd.DataFrame([bulk_row(r) for r in runs])
    return df.sort_values(["flows", "variant"]) if len(df) else df


def summarize_e2(runs: list[Run], floor_us: float | None = None) -> pd.DataFrame:
    """Target sweep. floor_us is the unloaded RTT; inferred queuing = RTT - floor."""
    rows = []
    for r in runs:
        row = bulk_row(r)
        row["base_target_us"] = _f(r.meta, "base_target_us")
        floor = floor_us if floor_us is not None else (
            r.trace["rtt_us"][r.trace["rtt_us"] > 0].min() if r.trace is not None else np.nan)
        row["inferred_queue_us_p50"] = row["rtt_us_p50"] - floor
        rows.append(row)
    df = pd.DataFrame(rows)
    return df.sort_values("base_target_us") if len(df) else df


def summarize_e3(runs: list[Run]) -> pd.DataFrame:
    rows = []
    for r in runs:
        row: dict[str, float | str] = {
            "run": r.name,
            "rep": _f(r.meta, "rep", 1),
            "variant": r.meta.get("variant", "?"),
            "fanin": _f(r.meta, "flows"),
            "rto_min": r.meta.get("rto_min", "default"),
            "retrans_pct": metrics.retrans_pct(r.nstat),
            "timeouts": r.nstat.get("TcpExtTCPTimeouts", np.nan),
        }
        if r.incast is not None:
            qct = metrics.round_completion_us(r.incast).to_numpy()
            row.update(metrics.pct_summary(qct, "qct_us"))
            row["fct_us_p99"] = float(np.percentile(r.incast["fct_us"], 99))
            row["rounds"] = len(qct)
        row.update(queue_stats(r))
        row["frac_ack_pct"] = swift_stats(r)["frac_ack_pct"]
        rows.append(row)
    df = pd.DataFrame(rows)
    return df.sort_values(["fanin", "variant"]) if len(df) else df


def spray_fraction(run: Run) -> float:
    """Measured share of bytes sent on the slow path.

    Uses the netem (slow) vs prio (all) qdisc byte counters. The iptables
    MARK count is not comparable to wire packets: it counts skbs before
    segmentation, whose size depends on the congestion control. Falls back to
    the old packet ratio for counter files without a netem line.
    """
    p = run.path / "spray_counters.txt"
    if not p.exists():
        return np.nan
    txt = p.read_text()
    total = re.search(r"qdisc prio 1: .*?\n Sent (\d+) bytes", txt, re.S)
    slow = re.search(r"qdisc netem \S+ parent 1:2.*?\n Sent (\d+) bytes", txt, re.S)
    if total and slow and int(total.group(1)):
        return int(slow.group(1)) / int(total.group(1))
    m = re.search(r"^\s*(\d+)\s+\d+\s+MARK", txt, re.M)
    s = re.search(r"qdisc prio 1: .*?\n Sent \d+ bytes (\d+) pkt", txt, re.S)
    if not m or not s or int(s.group(1)) == 0:
        return 0.0 if s else np.nan
    return int(m.group(1)) / int(s.group(1))


def summarize_e4(runs: list[Run]) -> pd.DataFrame:
    rows = []
    for r in runs:
        row = bulk_row(r)
        row["q"] = _f(r.meta, "q")
        row["reorder_tol"] = r.meta.get("reorder_tol", "default")
        row["q_measured"] = spray_fraction(r)
        row["reorder_events"] = sum(r.nstat.get(k, 0) for k in
                                    ("TcpExtTCPSACKReorder", "TcpExtTCPTSReorder",
                                     "TcpExtTCPRenoReorder"))
        row["fast_recoveries"] = r.nstat.get("TcpExtTCPSackRecovery", np.nan)
        rows.append(row)
    df = pd.DataFrame(rows)
    return df.sort_values(["reorder_tol", "variant", "q"]) if len(df) else df
