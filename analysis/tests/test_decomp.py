"""E7 pcap decomposition on a hand-built capture."""
import struct
from pathlib import Path

import pytest

from analysis import decomp


def _pkt(t: float, src: bytes, dst: bytes, sport: int, dport: int,
         seq: int, ack: int, plen: int) -> bytes:
    tcp = struct.pack(">HHIIBBHHH", sport, dport, seq, ack, 5 << 4, 0x10, 65535, 0, 0)
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + 20 + plen, 0, 0, 64, 6, 0, src, dst)
    eth = b"\0" * 12 + b"\x08\x00"
    frame = eth + ip + tcp  # payload not captured (snaplen)
    sec = int(t)
    usec = round((t - sec) * 1e6)
    return struct.pack("<IIII", sec, usec, len(frame), len(frame) + plen) + frame


def _write(path: Path, pkts: list[bytes]) -> None:
    gh = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 96, 1)
    path.write_bytes(gh + b"".join(pkts))


S, R = bytes([10, 10, 1, 1]), bytes([10, 10, 1, 2])


def test_ack_delay_matches_segment_holding_ack(tmp_path: Path) -> None:
    p = tmp_path / "tx.pcap"
    _write(p, [
        _pkt(1.000000, S, R, 40000, 5201, 1000, 1, 1448),   # ends 2448
        _pkt(1.000010, S, R, 40000, 5201, 2448, 1, 1448),   # ends 3896
        _pkt(1.000050, R, S, 5201, 40000, 1, 2448, 0),      # acks seg 1: 50 us
        _pkt(1.000080, R, S, 5201, 40000, 1, 3000, 0),      # inside seg 2: 70 us
        _pkt(1.000090, R, S, 5201, 40000, 1, 3000, 0),      # duplicate: ignored
    ])
    d = decomp.ack_delays(p)
    assert list(d["us"].round(1)) == [50.0, 70.0]


def test_retransmitted_segment_skipped(tmp_path: Path) -> None:
    p = tmp_path / "tx.pcap"
    _write(p, [
        _pkt(1.0, S, R, 40000, 5201, 1000, 1, 1448),
        _pkt(1.2, S, R, 40000, 5201, 1000, 1, 1448),        # retransmission
        _pkt(1.2001, R, S, 5201, 40000, 1, 2448, 0),
    ])
    assert len(decomp.ack_delays(p)) == 0


def test_decompose_subtracts_turnaround(tmp_path: Path) -> None:
    _write(tmp_path / "tx.pcap", [
        _pkt(1.000000, S, R, 40000, 5201, 1000, 1, 1448),
        _pkt(1.000060, R, S, 5201, 40000, 1, 2448, 0),      # 60 us at sender
    ])
    _write(tmp_path / "rx.pcap", [
        _pkt(5.000000, S, R, 40000, 5201, 1000, 1, 1448),   # other clock
        _pkt(5.000025, R, S, 5201, 40000, 1, 2448, 0),      # 25 us turnaround
    ])
    row = decomp.decompose(tmp_path)
    assert row["tx_rtt_p50"] == pytest.approx(60, abs=0.5)
    assert row["turnaround_p50"] == pytest.approx(25, abs=0.5)
    assert row["wire_queue_p50"] == pytest.approx(35, abs=0.5)
