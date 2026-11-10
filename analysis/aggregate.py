"""Collapse repeated runs (same configuration, different rep) into one row:
median, plus min and max as the spread. With 3-5 repeats, min-max is shown
rather than a standard deviation, which would imply a distribution we have
too few samples to estimate."""

import numpy as np
import pandas as pd


def has_repeats(df: pd.DataFrame, keys: list[str]) -> bool:
    """True when some configuration appears more than once."""
    if df.empty or not all(k in df.columns for k in keys):
        return False
    return bool(df.groupby(keys, dropna=False).size().max() > 1)


def collapse(df: pd.DataFrame, keys: list[str], metrics: list[str]) -> pd.DataFrame:
    """One row per configuration: n, <m> (median), <m>_lo, <m>_hi for each metric."""
    present = [m for m in metrics if m in df.columns]
    rows = []
    for cfg, sub in df.groupby(keys, dropna=False, sort=True):
        cfg = cfg if isinstance(cfg, tuple) else (cfg,)
        row: dict[str, object] = dict(zip(keys, cfg))
        row["n"] = len(sub)
        for m in present:
            x = pd.to_numeric(sub[m], errors="coerce").dropna().to_numpy(dtype=float)
            row[m] = float(np.median(x)) if len(x) else np.nan
            row[f"{m}_lo"] = float(x.min()) if len(x) else np.nan
            row[f"{m}_hi"] = float(x.max()) if len(x) else np.nan
        rows.append(row)
    return pd.DataFrame(rows)


def _num(v: float) -> str:
    return f"{v:.3g}"


def range_cell(row: pd.Series, m: str) -> str:
    """'median (lo-hi)' for a collapsed row, or the plain value."""
    v = row.get(m, np.nan)
    if v != v:
        return ""
    lo, hi = row.get(f"{m}_lo", np.nan), row.get(f"{m}_hi", np.nan)
    if lo != lo or hi != hi:
        return _num(v)
    return f"{_num(v)} ({_num(lo)}-{_num(hi)})"
