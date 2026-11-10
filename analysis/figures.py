"""Report figures. Each function writes one PNG and returns its path.

Colors follow the entity (algorithm), never its rank, using the validated
categorical palette in fixed slot order.
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

from analysis.loaders import Run  # noqa: E402

COLORS = {
    "swift": "#2a78d6", "cubic": "#eb6834", "dctcp": "#1baf7a", "vegas": "#eda100",
    "bbr": "#e87ba4", "mswift": "#008300", "swift-nofrac": "#6250d6",
    "mswift-k4": "#e34948", "swift-fs": "#52514e",
    # Swift pacing variants: shades of Swift's hue, so the family reads as one.
    "swift-pmin": "#9ec5f0", "swift-pcap": "#1d5fae",
}
INK, MUTED = "#0b0b0b", "#52514e"

plt.rcParams.update({
    "figure.dpi": 150, "savefig.dpi": 200, "font.size": 9,
    "axes.edgecolor": MUTED, "axes.labelcolor": INK, "axes.titlesize": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6,
    "xtick.color": MUTED, "ytick.color": MUTED, "legend.frameon": False,
    "lines.linewidth": 1.5, "lines.markersize": 5,
})


def _color(variant: str) -> str:
    return COLORS.get(variant, MUTED)


def _plain_log(ax: plt.Axes, axis: str = "x") -> None:
    """Log scale with plain-number labels and no minor labels (avoids overlap)."""
    from matplotlib.ticker import LogLocator, NullFormatter, ScalarFormatter
    a = ax.xaxis if axis == "x" else ax.yaxis
    (ax.set_xscale if axis == "x" else ax.set_yscale)("log")
    a.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
    a.set_major_formatter(ScalarFormatter())
    a.set_minor_formatter(NullFormatter())


def _err(sub: pd.DataFrame, col: str) -> np.ndarray | None:
    """Asymmetric min-max error bars for a collapsed table, else None."""
    lo, hi = f"{col}_lo", f"{col}_hi"
    if lo not in sub or hi not in sub:
        return None
    v = sub[col].to_numpy(dtype=float)
    return np.vstack([v - sub[lo].to_numpy(dtype=float), sub[hi].to_numpy(dtype=float) - v])


def _line(ax: plt.Axes, sub: pd.DataFrame, x: str, y: str, scale: float = 1.0,
          **kw: object) -> None:
    """Line with markers, plus min-max bars when the table has repeats."""
    e = _err(sub, y)
    ax.errorbar(sub[x], sub[y] / scale, yerr=None if e is None else e / scale,
                marker=kw.pop("marker", "o"), capsize=2.5, elinewidth=1, **kw)


def _save(fig: plt.Figure, out: Path) -> Path:
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)
    return out


def e0_rtt_cdf(runs: list[Run], out: Path) -> Path | None:
    shades = ["#9ec5f0", "#5a9be3", "#1d5fae"]  # one hue, light -> dark = load
    fig, ax = plt.subplots(figsize=(5, 3.2))
    drawn = 0
    for run, c in zip(runs, shades):
        if run.trace is None:
            continue
        x = np.sort(run.trace.loc[run.trace["rtt_us"] > 0, "rtt_us"].to_numpy())
        if not len(x):
            continue
        ax.plot(x, np.linspace(0, 1, len(x)), color=c,
                label=f"load {run.meta.get('offered_load', run.name)}")
        drawn += 1
    if not drawn:
        plt.close(fig)
        return None
    _plain_log(ax)
    ax.set_xlabel("per-ACK RTT sample (µs, log)")
    ax.set_ylabel("CDF")
    ax.set_title("RTT seen by the module with no bottleneck")
    ax.legend()
    return _save(fig, out)


def e1_tradeoff(df: pd.DataFrame, out: Path) -> Path | None:
    if df.empty:
        return None
    flows = sorted(df["flows"].dropna().unique())
    fig, axes = plt.subplots(1, len(flows), figsize=(4.2 * len(flows), 3.2), squeeze=False)
    for ax, n in zip(axes[0], flows):
        sub = df[df["flows"] == n]
        xe, ye = _err(sub, "rpc_us_p99"), _err(sub, "goodput_gbps")
        for i, (_, r) in enumerate(sub.iterrows()):
            if xe is not None and ye is not None:
                ax.errorbar(r["rpc_us_p99"], r["goodput_gbps"], xerr=xe[:, i:i + 1],
                            yerr=ye[:, i:i + 1], color=_color(r["variant"]), capsize=2.5,
                            elinewidth=1, zorder=2)
            ax.scatter(r["rpc_us_p99"], r["goodput_gbps"], s=40, color=_color(r["variant"]),
                       edgecolor="white", linewidth=1.5, zorder=3)
            ax.annotate(r["variant"], (r["rpc_us_p99"], r["goodput_gbps"]),
                        textcoords="offset points", xytext=(6, 3), color=INK, fontsize=8)
        _plain_log(ax)
        ax.set_xlabel("p99 RPC latency under load (µs, log)")
        ax.set_ylabel("goodput (Gb/s)")
        ax.set_title(f"{int(n)} flow{'s' if n > 1 else ''}")
        ax.set_ylim(bottom=0)
    return _save(fig, out)


def e1_queue_bars(df: pd.DataFrame, out: Path) -> Path | None:
    if df.empty:
        return None
    flows = sorted(df["flows"].dropna().unique())
    fig, axes = plt.subplots(1, len(flows), figsize=(4.2 * len(flows), 3.0), squeeze=False)
    for ax, n in zip(axes[0], flows):
        sub = df[df["flows"] == n].sort_values("queue_us_p99")
        y = np.arange(len(sub))
        ax.barh(y, sub["queue_us_p99"], color=[_color(v) for v in sub["variant"]],
                height=0.6, edgecolor="white", linewidth=2, xerr=_err(sub, "queue_us_p99"),
                error_kw={"ecolor": INK, "capsize": 2.5, "elinewidth": 1})
        ax.set_yticks(y, sub["variant"])
        ax.set_xlabel("bottleneck queuing delay, p99 (µs)")
        ax.set_title(f"{int(n)} flow{'s' if n > 1 else ''}")
        ax.grid(axis="y", visible=False)
    return _save(fig, out)


def swift_timeseries(run: Run, out: Path, window_s: float = 0.5) -> Path | None:
    """cwnd and delay-vs-target for the busiest flow, over a short window."""
    t = run.trace
    if t is None or t.empty:
        return None
    flow = t.groupby("dport").size().idxmax()
    f = t[(t["dport"] == flow) & (t["rtt_us"] > 0)].copy()
    f["t_s"] = (f["ts_ns"] - f["ts_ns"].min()) / 1e9
    mid = f["t_s"].max() / 2
    f = f[(f["t_s"] >= mid) & (f["t_s"] < mid + window_s)]
    if f.empty:
        return None
    f["t_ms"] = (f["t_s"] - mid) * 1e3
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(6, 4), sharex=True)
    a1.plot(f["t_ms"], f["cwnd"], color=_color("swift"))
    a1.set_ylabel("cwnd (packets)")
    a1.set_title(f"Swift, one flow of {run.name}")
    a2.plot(f["t_ms"], f["rtt_us"], color="#9ec5f0", linewidth=0.8, label="RTT sample")
    a2.plot(f["t_ms"], f["delay_us"], color=_color("swift"), linewidth=1.2, label="delay used")
    a2.plot(f["t_ms"], f["target_us"], color=INK, linewidth=1.2, linestyle="--", label="target")
    a2.set_ylabel("µs")
    a2.set_xlabel("time (ms)")
    a2.legend(ncol=3, loc="lower left", bbox_to_anchor=(0, 1.0), borderaxespad=0.2)
    return _save(fig, out)


def e2_target_sweep(df: pd.DataFrame, out: Path) -> Path | None:
    if df.empty:
        return None
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(5, 4.6), sharex=True)
    x = df["base_target_us"]
    _line(a1, df, "base_target_us", "util_pct", color=_color("swift"))
    a1.set_ylabel("bottleneck utilization (%)")
    a1.set_ylim(0, 105)
    a1.set_title("Swift base-target sweep")
    _line(a2, df, "base_target_us", "rtt_us_p50", color=_color("swift"), label="module RTT p50")
    _line(a2, df, "base_target_us", "queue_us_p50", marker="s", color="#5a9be3",
          label="queue delay p50")
    _line(a2, df, "base_target_us", "queue_us_p99", marker="^", color="#1d5fae",
          label="queue delay p99")
    a2.plot(x, x, color=MUTED, linestyle=":", linewidth=1, label="y = target")
    a2.set_xlabel("base target (µs)")
    a2.set_ylabel("µs")
    a2.legend()
    return _save(fig, out)


def e3_incast(df: pd.DataFrame, out: Path) -> Path | None:
    """p99 round completion and retransmissions vs fan-in; one column per
    minimum-RTO setting so the two regimes are not overlaid."""
    if df.empty or "qct_us_p99" not in df:
        return None
    df = df.assign(rto_min=df["rto_min"] if "rto_min" in df else "default")
    rtos = sorted(df["rto_min"].unique(), key=lambda r: (r != "default", r))
    fig, axes = plt.subplots(2, len(rtos), figsize=(4.2 * len(rtos), 5.4), squeeze=False,
                             sharex=True, sharey="row")
    ticks = sorted(df["fanin"].dropna().unique())
    handles = {}
    for j, r in enumerate(rtos):
        part = df[df["rto_min"] == r]
        for v, sub in part.groupby("variant"):
            sub = sub.sort_values("fanin")
            _line(axes[0][j], sub, "fanin", "qct_us_p99", scale=1e3, color=_color(v), label=v)
            _line(axes[1][j], sub, "fanin", "retrans_pct", color=_color(v), label=v)
            handles[v] = plt.Line2D([], [], color=_color(v), marker="o")
        axes[0][j].set_title("Linux default min RTO (200 ms)" if r == "default"
                             else f"min RTO {r} (route rto_min)")
        _plain_log(axes[0][j], "y")
        for a in axes[:, j]:
            a.set_xscale("log", base=2)
            a.set_xticks(ticks, [str(int(t)) for t in ticks])
            a.minorticks_off()
            a.set_xlim(min(ticks) / 1.25, max(ticks) * 1.25)
        axes[1][j].set_xlabel("fan-in (flows)")
    axes[0][0].set_ylabel("p99 round completion (ms, log)")
    axes[1][0].set_ylabel("retransmitted segments (%)")
    names = sorted(handles)
    fig.legend([handles[n] for n in names], names, loc="lower center", ncol=len(names),
               bbox_to_anchor=(0.5, 0), frameon=False)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    fig.savefig(out)
    plt.close(fig)
    return out


def e4_spray(df: pd.DataFrame, out: Path) -> Path | None:
    """Goodput vs q; one panel per loss-recovery setting (reorder_tol)."""
    if df.empty:
        return None
    tols = (sorted(df["reorder_tol"].unique(), key=lambda t: (t != "default", t))
            if "reorder_tol" in df else [None])
    fig, axes = plt.subplots(1, len(tols), figsize=(5 * len(tols), 3.2), squeeze=False,
                             sharey=True)
    for ax, tol in zip(axes[0], tols):
        part = df if tol is None else df[df["reorder_tol"] == tol]
        for v, sub in part.groupby("variant"):
            _line(ax, sub.sort_values("q"), "q", "goodput_gbps", color=_color(v), label=v)
        ax.set_xlabel("fraction of packets on the slow path, q")
        ax.set_ylim(bottom=0)
        ax.set_title("Linux default loss recovery" if tol in (None, "default")
                     else f"reordering tolerant (dupthresh {tol}, RACK off)")
        ax.legend()
    axes[0][0].set_ylabel("goodput (Gb/s)")
    return _save(fig, out)
