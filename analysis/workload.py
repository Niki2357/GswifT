"""E8: slowdown versus message size for the open-loop RPC workloads.

    python3 -m analysis.workload results/2026-11-20

Slowdown of an RPC = its latency / the best case for its size, where the best
case is the session's unloaded 64 B RPC latency (p50, from host baselines)
plus the response's transmission time at the bottleneck rate. As in
Homa/Linux (Figure 3), RPCs are grouped by size quantile, so the x-axis is the
cumulative fraction of RPCs.

Writes tables/e8.csv (per run, per size bin), tables/e8_summary.csv (per
configuration, short/medium/long), figs/e8_slowdown_<workload>.png.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from analysis.loaders import load_meta

N_BINS = 20
SHORT_MAX, LONG_MIN = 10_000, 1_000_000  # bytes


def base_rtt_us(root: Path) -> float:
    """Median of the per-repeat unloaded 64 B RPC p50s (host/*/baseline.txt)."""
    vals = []
    for p in sorted((root / "host").glob("*/baseline.txt")) + [root / "session/baseline.txt"]:
        m = re.search(r"p50=([\d.]+)", p.read_text()) if p.exists() else None
        if m:
            vals.append(float(m.group(1)))
    return float(np.median(vals)) if vals else 40.0


def _rate_bps(meta: dict[str, str]) -> float:
    m = re.match(r"([\d.]+)([gmk]?)bit", meta.get("bn_rate", "10gbit").lower())
    return float(m[1]) * {"g": 1e9, "m": 1e6, "k": 1e3, "": 1}[m[2]] if m else 10e9


def load_runs(root: Path, base_us: float) -> pd.DataFrame:
    frames = []
    for run in sorted((root / "e8_workload").glob("*")):
        f = run / "rpc.csv"
        if not f.exists() or f.stat().st_size == 0:
            continue
        meta = load_meta(run / "meta.txt")
        df = pd.read_csv(f)
        rate = _rate_bps(meta)
        df["ideal_us"] = base_us + df["size"] * 8 / rate * 1e6
        df["slowdown"] = df["latency_us"] / df["ideal_us"]
        df["workload"] = meta.get("workload", "?")
        df["load"] = float(meta.get("load", "nan"))
        df["variant"] = meta.get("variant", "?")
        df["bn"] = meta.get("bn_kind", "ifb")
        df["conns"] = int(float(meta.get("conns", 128)))
        df["rep"] = int(float(meta.get("rep", 1)))
        frames.append(df)
    return pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()


def by_size_bin(df: pd.DataFrame) -> pd.DataFrame:
    """P50/P99 slowdown per size-quantile bin, per configuration (repeats pooled)."""
    rows = []
    for (bn, c, w, load, v), g in df.groupby(["bn", "conns", "workload", "load", "variant"]):
        g = g.sort_values("size", kind="stable").reset_index(drop=True)
        edges = np.linspace(0, len(g), N_BINS + 1).astype(int)
        for i in range(N_BINS):
            b = g.iloc[edges[i]:edges[i + 1]]
            if len(b):
                rows.append({"bn": bn, "conns": c, "workload": w, "load": load, "variant": v,
                             "bin": i,
                             "cum_pct": 100 * edges[i + 1] / len(g),
                             "size_max": int(b["size"].max()), "n": len(b),
                             "p50": b["slowdown"].quantile(0.5),
                             "p99": b["slowdown"].quantile(0.99)})
    return pd.DataFrame(rows)


def summary(df: pd.DataFrame) -> pd.DataFrame:
    """Short (< 10 KB), medium, long (>= 1 MB) RPCs: P50/P99 slowdown, with the
    range of per-repeat P99s."""
    df = df.assign(cls=pd.cut(df["size"], [0, SHORT_MAX, LONG_MIN - 1, np.inf],
                              labels=["short", "medium", "long"], right=False))
    rows = []
    for (bn, nc, w, load, v, c), g in df.groupby(
            ["bn", "conns", "workload", "load", "variant", "cls"], observed=True):
        per_rep = g.groupby("rep")["slowdown"].quantile(0.99)
        rows.append({"bn": bn, "conns": nc, "workload": w, "load": load, "variant": v,
                     "class": c,
                     "n": len(g),
                     "p50": g["slowdown"].median(), "p99": g["slowdown"].quantile(0.99),
                     "p99_lo": per_rep.min(), "p99_hi": per_rep.max()})
    return pd.DataFrame(rows)


def plot(bins: pd.DataFrame, out_dir: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from analysis.figures import _color as color  # same per-variant colors

    out_dir.mkdir(exist_ok=True)
    for (bn, w), gw in bins[bins["conns"] == 128].groupby(["bn", "workload"]):
        loads = sorted(gw["load"].unique())
        fig, axes = plt.subplots(1, len(loads), figsize=(5 * len(loads), 3.6),
                                 sharey=True, squeeze=False)
        for ax, load in zip(axes[0], loads):
            for v, g in gw[gw["load"] == load].groupby("variant"):
                ax.plot(g["cum_pct"], g["p99"], color=color(v), lw=2, label=f"{v} P99")
                ax.plot(g["cum_pct"], g["p50"], color=color(v), lw=1, ls="--",
                        label=f"{v} P50")
            ax.set_yscale("log")
            ax.set_title(f"{w}, load {load:.0%} ({bn} bottleneck)")
            ax.set_xlabel("cumulative % of RPCs (by size)")
            ax.grid(alpha=0.3, which="both")
        axes[0][0].set_ylabel("slowdown")
        handles, labels = axes[0][0].get_legend_handles_labels()
        fig.legend(handles, labels, loc="lower center", ncol=4, fontsize=8, frameon=False)
        fig.tight_layout(rect=(0, 0.14, 1, 1))
        fig.savefig(out_dir / f"e8_slowdown_{w}_{bn}.png", dpi=150)
        plt.close(fig)


def main(argv: list[str]) -> int:
    root = Path(argv[0])
    base = base_rtt_us(root)
    df = load_runs(root, base)
    if not len(df):
        print("no e8 runs")
        return 1
    (root / "tables").mkdir(exist_ok=True)
    bins = by_size_bin(df)
    bins.to_csv(root / "tables/e8.csv", index=False)
    summ = summary(df)
    summ.to_csv(root / "tables/e8_summary.csv", index=False)
    plot(bins, root / "figs")
    print(f"base RTT {base:.1f} us")
    print(summ.round(2).to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
