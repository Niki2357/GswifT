"""Tests for collapsing repeated runs into median and range per configuration."""

import numpy as np
import pandas as pd
import pytest

from analysis import aggregate


def _df() -> pd.DataFrame:
    return pd.DataFrame({
        "variant": ["swift"] * 3 + ["cubic"] * 2,
        "flows": [8.0] * 5,
        "rep": [1, 2, 3, 1, 2],
        "util_pct": [22.0, 53.0, 35.0, 50.0, 54.0],
        "note": ["a", "b", "c", "d", "e"],
    })


def test_median_and_range_per_config() -> None:
    out = aggregate.collapse(_df(), ["variant", "flows"], ["util_pct"])
    sw = out[out["variant"] == "swift"].iloc[0]
    assert sw["n"] == 3
    assert sw["util_pct"] == pytest.approx(35.0)
    assert sw["util_pct_lo"] == pytest.approx(22.0)
    assert sw["util_pct_hi"] == pytest.approx(53.0)
    cu = out[out["variant"] == "cubic"].iloc[0]
    assert cu["n"] == 2 and cu["util_pct"] == pytest.approx(52.0)


def test_missing_metric_is_skipped_and_nan_ignored() -> None:
    df = _df()
    df.loc[0, "util_pct"] = np.nan
    out = aggregate.collapse(df, ["variant"], ["util_pct", "absent"])
    sw = out[out["variant"] == "swift"].iloc[0]
    assert sw["util_pct"] == pytest.approx(44.0)
    assert "absent" not in out.columns


def test_has_repeats() -> None:
    assert aggregate.has_repeats(_df(), ["variant", "flows"])
    single = _df().drop_duplicates(["variant"])
    assert not aggregate.has_repeats(single, ["variant", "flows"])
    assert not aggregate.has_repeats(pd.DataFrame(), ["variant"])


def test_range_cell() -> None:
    row = pd.Series({"x": 35.0, "x_lo": 22.0, "x_hi": 53.0})
    assert aggregate.range_cell(row, "x") == "35 (22-53)"
    assert aggregate.range_cell(pd.Series({"x": 1.5}), "x") == "1.5"
