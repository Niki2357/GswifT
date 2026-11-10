"""Summarize one session's results into tables and figures.

    python3 -m analysis.analyze results/2026-11-09

Writes <dir>/summary.md (markdown tables per experiment), <dir>/tables/*.csv
and <dir>/figs/*.png. Experiments that were not run are skipped.
"""

import argparse
import re
from pathlib import Path

import pandas as pd

from analysis import aggregate, figures, summaries
from analysis.loaders import cumulative_to_delta, load_experiment

COLS = {
    "e0": ["load", "goodput_gbps", "samples", "rtt_us_min", "rtt_us_p50", "rtt_us_p99",
           "rtt_us_p999", "excess_us_p50", "excess_us_p99"],
    "e1": ["variant", "flows", "goodput_gbps", "util_pct", "queue_us_p50", "queue_us_p99",
           "rpc_us_p50", "rpc_us_p99", "retrans_pct", "jain", "rtt_us_p50", "target_us",
           "decr_per_s"],
    "e2": ["base_target_us", "goodput_gbps", "util_pct", "rtt_us_p50", "rtt_us_p99",
           "inferred_queue_us_p50", "queue_us_p50", "queue_us_p99", "rpc_us_p99",
           "decr_per_s", "retrans_pct"],
    "e3": ["variant", "rto_min", "fanin", "qct_us_p50", "qct_us_p99", "fct_us_p99", "retrans_pct",
           "timeouts", "drops", "queue_us_p99", "frac_ack_pct"],
    "e4": ["variant", "reorder_tol", "q", "q_measured", "goodput_gbps", "util_pct", "retrans_pct",
           "reorder_events", "fast_recoveries", "decr_per_s", "rtt_us_p50"],
}


# Configuration columns (repeats share them) and the metrics whose spread
# across repeats is reported.
KEYS = {
    "e1": ["flows", "variant"],
    "e2": ["base_target_us"],
    "e3": ["rto_min", "fanin", "variant"],
    "e4": ["reorder_tol", "variant", "q"],
}
SPREAD = {
    "e1": ["goodput_gbps", "util_pct", "queue_us_p99", "rpc_us_p99", "retrans_pct",
           "rtt_us_p50", "decr_per_s", "jain"],
    "e2": ["util_pct", "goodput_gbps", "rtt_us_p50", "rtt_us_p99", "queue_us_p50",
           "queue_us_p99", "rpc_us_p99", "decr_per_s"],
    "e3": ["qct_us_p50", "qct_us_p99", "retrans_pct", "timeouts", "drops", "frac_ack_pct"],
    "e4": ["q_measured", "goodput_gbps", "util_pct", "retrans_pct", "fast_recoveries",
           "decr_per_s", "rtt_us_p50"],
}


def _md_spread(df: pd.DataFrame, keys: list[str], metrics: list[str]) -> str:
    """Markdown table of collapsed repeats: 'median (min-max)' per cell."""
    present = [m for m in metrics if m in df.columns]
    head = keys + ["n"] + present
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for _, row in df.iterrows():
        cells = [_cell(row[k]) for k in keys] + [str(int(row["n"]))]
        cells += [aggregate.range_cell(row, m) for m in present]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def _cell(v: object) -> str:
    if isinstance(v, float):
        return "" if v != v else f"{v:.3g}"
    return str(v)


def _md(df: pd.DataFrame, cols: list[str]) -> str:
    """Markdown table without the optional 'tabulate' dependency."""
    present = [c for c in cols if c in df.columns]
    if not len(df) or not present:
        return "_no runs_"
    lines = ["| " + " | ".join(present) + " |", "|" + "---|" * len(present)]
    for _, row in df[present].iterrows():
        lines.append("| " + " | ".join(_cell(row[c]) for c in present) + " |")
    return "\n".join(lines)


def unloaded_floor_us(root: Path) -> float | None:
    """p1 of the session's unloaded 64 B RPC RTT, used as the no-queue floor."""
    p = root / "session" / "baseline.txt"
    if not p.exists():
        return None
    m = re.search(r"p1=([0-9.]+)", p.read_text())
    return float(m.group(1)) if m else None


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root", type=Path, help="session results directory")
    root = ap.parse_args().root
    figs, tables = root / "figs", root / "tables"
    figs.mkdir(exist_ok=True)
    tables.mkdir(exist_ok=True)
    sections: list[str] = [f"# Results summary: {root.name}\n"]

    for f in ("nodes.txt", "baseline.txt"):
        p = root / "session" / f
        if p.exists():
            sections.append(f"## Session {f}\n\n```\n{p.read_text().strip()}\n```\n")

    def emit(key: str, title: str, df: pd.DataFrame) -> pd.DataFrame:
        """Write the per-run table; with repeats, also the collapsed one.
        Returns what the figures should plot (collapsed when repeated)."""
        df.to_csv(tables / f"{key}.csv", index=False)
        keys = KEYS.get(key)
        if keys and aggregate.has_repeats(df, keys):
            agg = aggregate.collapse(df, keys, SPREAD[key])
            agg.to_csv(tables / f"{key}_agg.csv", index=False)
            sections.append(f"## {title}\n\nMedian (min-max) over n repeats.\n\n"
                            f"{_md_spread(agg, keys, SPREAD[key])}\n\n"
                            f"<details><summary>Every run</summary>\n\n"
                            f"{_md(df, ['run', 'rep'] + COLS[key])}\n\n</details>\n")
            return agg
        sections.append(f"## {title}\n\n{_md(df, COLS[key])}\n")
        return df

    exps = {k: load_experiment(root / k) for k in
            ("smoke", "e0_noise", "e1_compare", "e2_target", "e3_incast", "e4_spray", "e5_diag")}
    every = [r for runs in exps.values() for r in runs]
    fixed = {r.path: r for r in cumulative_to_delta(every)}
    exps = {k: [fixed[r.path] for r in runs] for k, runs in exps.items()}

    e0 = exps["e0_noise"]
    if e0:
        emit("e0", "E0: RTT noise floor (no bottleneck)", summaries.summarize_e0(e0))
        figures.e0_rtt_cdf(e0, figs / "e0_rtt_cdf.png")

    e1 = exps["e1_compare"]
    if e1:
        df = emit("e1", "E1: algorithm comparison at the bottleneck",
                  summaries.summarize_bulk(e1))
        figures.e1_tradeoff(df, figs / "e1_tradeoff.png")
        figures.e1_queue_bars(df, figs / "e1_queue.png")
        for r in e1:
            if r.meta.get("variant") == "swift" and r.meta.get("rep", "1") == "1":
                figures.swift_timeseries(r, figs / f"e1_trace_{r.name}.png")

    e2 = exps["e2_target"]
    if e2:
        df = emit("e2", "E2: base-target sweep (8 flows)",
                  summaries.summarize_e2(e2, unloaded_floor_us(root)))
        figures.e2_target_sweep(df, figs / "e2_target.png")

    e3 = exps["e3_incast"]
    if e3:
        df = emit("e3", "E3: incast fan-in sweep", summaries.summarize_e3(e3))
        figures.e3_incast(df, figs / "e3_incast.png")

    e4 = exps["e4_spray"]
    if e4:
        df = emit("e4", "E4: spraying with one slow path", summaries.summarize_e4(e4))
        figures.e4_spray(df, figs / "e4_spray.png")

    (root / "summary.md").write_text("\n".join(sections))
    print(f"wrote {root / 'summary.md'}, {len(list(figs.glob('*.png')))} figures")


if __name__ == "__main__":
    main()
