"""E9 cost parsing on hand-made counter files."""
import json
from pathlib import Path

import pytest

from analysis import overhead


def test_cost_row_cores_and_bpf(tmp_path: Path) -> None:
    (tmp_path / "meta.txt").write_text("ncpu_sender=4\nncpu_receiver=4\n")
    # user nice system idle iowait irq softirq: 100 busy of 400 total -> 1 core
    (tmp_path / "cpu_sender.txt").write_text(
        "cpu 0 0 0 0 0 0 0\ncpu 50 0 30 300 0 0 20\n")
    (tmp_path / "cpu_receiver.txt").write_text(
        "cpu 0 0 0 0 0 0 0\ncpu 100 0 100 200 0 0 0\n")
    (tmp_path / "bpf_before.json").write_text(json.dumps(
        [{"name": "swift_cong_control", "run_time_ns": 1000, "run_cnt": 10}]))
    (tmp_path / "bpf_after.json").write_text(json.dumps(
        [{"name": "swift_cong_control", "run_time_ns": 21000, "run_cnt": 110},
         {"name": "other", "run_time_ns": 5, "run_cnt": 5}]))
    row = overhead.cost_row(tmp_path)
    assert row["sender_cores"] == pytest.approx(1.0)
    assert row["receiver_cores"] == pytest.approx(2.0)
    assert row["ns_per_cong_control"] == pytest.approx(200.0)
    assert "ns_per_other" not in row
