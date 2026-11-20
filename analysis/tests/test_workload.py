"""E8 slowdown computation on a synthetic run."""
from pathlib import Path

import pandas as pd
import pytest

from analysis import workload


def test_slowdown_and_bins(tmp_path: Path) -> None:
    (tmp_path / "host/base-r1").mkdir(parents=True)
    (tmp_path / "host/base-r1/baseline.txt").write_text("tcp_rr_64B_us n=1 p1=1 p50=40.0 p99=1\n")
    run = tmp_path / "e8_workload/rpcmix-l0.5-swift"
    run.mkdir(parents=True)
    (run / "meta.txt").write_text("bn_rate=10gbit\nworkload=rpcmix\nload=0.5\nvariant=swift\nrep=1\n")
    # 1250 B at 10 Gb/s = 1 us, ideal 41 us; 1.25 MB = 1000 us, ideal 1040 us.
    pd.DataFrame({"size": [1250, 1250, 1_250_000], "latency_us": [41.0, 82.0, 2080.0],
                  "wait_us": [0, 0, 0]}).to_csv(run / "rpc.csv", index=False)
    base = workload.base_rtt_us(tmp_path)
    assert base == pytest.approx(40.0)
    df = workload.load_runs(tmp_path, base)
    assert list(df["slowdown"].round(3)) == [1.0, 2.0, 2.0]
    s = workload.summary(df).set_index("class")
    assert s.loc["short", "p50"] == pytest.approx(1.5)
    assert s.loc["long", "n"] == 1
    assert len(workload.by_size_bin(df)) == 3
