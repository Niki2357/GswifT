"""Readers for the files each experiment run directory contains.

A run directory (results/<date>/<exp>/<run>/) may hold:
  meta.txt          key=value run description (always)
  iperf.json        iperf3 -J output
  trace.csv         swiftctl per-ACK telemetry (Swift runs)
  swift_stats.txt   swiftctl counters at exit (Swift runs)
  qmon.csv          bottleneck backlog samples
  probe.csv         64 B RPC latency samples under load
  incast.csv        per-flow completion times (incast runs)
  nstat.txt         sender TCP counters for the run
Every reader returns None / empty when its file is missing or unusable, so a
partially failed run still summarizes.
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd


@dataclass(frozen=True)
class IperfResult:
    goodput_bps: float
    retransmits: int
    per_flow_bps: tuple[float, ...]


@dataclass(frozen=True)
class Run:
    name: str
    path: Path
    meta: dict[str, str]
    iperf: IperfResult | None
    trace: pd.DataFrame | None
    stats: dict[str, str]
    qmon: pd.DataFrame | None
    probe: pd.DataFrame | None
    incast: pd.DataFrame | None
    nstat: dict[str, float] = field(default_factory=dict)


def load_kv(path: Path) -> dict[str, str]:
    """key=value pairs, several per line allowed."""
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
    return out


def load_meta(path: Path) -> dict[str, str]:
    """meta.txt: one key=value per line; values may contain spaces."""
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def load_nstat(path: Path) -> dict[str, float]:
    out: dict[str, float] = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2 and not line.startswith("#"):
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                continue
    return out


def load_iperf(path: Path) -> IperfResult | None:
    try:
        doc = json.loads(path.read_text())
        end = doc["end"]
        flows = tuple(float(s["receiver"]["bits_per_second"]) for s in end.get("streams", []))
        return IperfResult(goodput_bps=float(end["sum_received"]["bits_per_second"]),
                           retransmits=int(end.get("sum_sent", {}).get("retransmits", 0)),
                           per_flow_bps=flows)
    except (OSError, ValueError, KeyError, TypeError):
        return None


def load_csv(path: Path) -> pd.DataFrame | None:
    """CSV reader tolerant of a truncated last line (process killed mid-write)."""
    if not path.exists() or path.stat().st_size == 0:
        return None
    try:
        df = pd.read_csv(path, on_bad_lines="skip")
    except (pd.errors.EmptyDataError, pd.errors.ParserError):
        return None
    return df.dropna() if len(df) else None


def load_trace(path: Path) -> pd.DataFrame | None:
    return load_csv(path)


def load_run(path: Path) -> Run:
    return Run(
        name=path.name,
        path=path,
        meta=load_meta(path / "meta.txt"),
        iperf=load_iperf(path / "iperf.json"),
        trace=load_trace(path / "trace.csv"),
        stats=load_kv(path / "swift_stats.txt"),
        qmon=load_csv(path / "qmon.csv"),
        probe=load_csv(path / "probe.csv"),
        incast=load_csv(path / "incast.csv"),
        nstat=load_nstat(path / "nstat.txt"),
    )


def load_experiment(exp_dir: Path) -> list[Run]:
    """All runs of one experiment, sorted by name."""
    if not exp_dir.is_dir():
        return []
    return [load_run(d) for d in sorted(exp_dir.iterdir()) if (d / "meta.txt").exists()]
