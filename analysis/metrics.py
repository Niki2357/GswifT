"""Pure metric helpers shared by the experiment summaries."""

import re
from typing import Iterable

import numpy as np
import pandas as pd

_RATE_UNITS = {"": 1.0, "k": 1e3, "m": 1e6, "g": 1e9, "t": 1e12}


def parse_rate_bps(rate: str) -> float:
    """Parse a tc-style rate such as '10gbit' or '500mbit' into bits/s."""
    m = re.fullmatch(r"\s*([0-9.]+)\s*([kmgt]?)bit\s*", rate.lower())
    if not m:
        raise ValueError(f"unrecognized rate '{rate}'")
    return float(m.group(1)) * _RATE_UNITS[m.group(2)]


def queue_delay_us(backlog_bytes: float | np.ndarray, rate_bps: float) -> float | np.ndarray:
    """Time to drain a backlog at the bottleneck rate."""
    return backlog_bytes * 8.0 / rate_bps * 1e6


def jain(values: Iterable[float]) -> float:
    """Jain's fairness index: 1 = perfectly fair, 1/n = one flow gets all."""
    x = np.asarray(list(values), dtype=float)
    if x.size == 0 or not np.any(x):
        return float("nan")
    return float(x.sum() ** 2 / (x.size * (x ** 2).sum()))


def pct_summary(values: np.ndarray, prefix: str) -> dict[str, float]:
    """p50, p99, p99.9 and mean of a sample, keyed with a prefix."""
    x = np.asarray(values, dtype=float)
    keys = [f"{prefix}_p50", f"{prefix}_p99", f"{prefix}_p999", f"{prefix}_mean"]
    if x.size == 0:
        return {k: float("nan") for k in keys}
    p50, p99, p999 = np.percentile(x, [50, 99, 99.9])
    return dict(zip(keys, (float(p50), float(p99), float(p999), float(x.mean()))))


def round_completion_us(incast: pd.DataFrame) -> pd.Series:
    """Per-round completion time: the slowest flow in each round."""
    return incast.groupby("round")["fct_us"].max()


def retrans_pct(nstat: dict[str, float]) -> float:
    out = nstat.get("TcpOutSegs", 0)
    return 100.0 * nstat.get("TcpRetransSegs", 0) / out if out else float("nan")
