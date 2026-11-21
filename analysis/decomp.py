"""E7: split the sender's RTT samples into host and network parts.

Inputs per run directory (scripts/run_e7_decomp.sh): tx.pcap (sender),
rx.pcap (receiver), trace.csv (module samples). Each pcap is only compared
with itself, so the two hosts' clocks need not agree:

  tx_rtt      ACK seen at sender NIC tap - data seen at sender NIC tap
  turnaround  ACK seen at receiver tap - data seen at receiver tap
  wire_queue  tx_rtt - turnaround (same flow, same ACK number)
  module      the module's rate_sample RTT (includes sender stack and qdisc)

Usage: python3 -m analysis.decomp RUN_DIR [RUN_DIR ...]
"""
from __future__ import annotations

import bisect
import struct
import sys
from pathlib import Path

import numpy as np
import pandas as pd

PCTS = (10, 50, 90, 99)
DATA_PORTS = range(5201, 5265)  # one iperf3 port per flow


def read_pcap(path: Path):
    """Yields (t_s, src_ip, sport, dport, seq, ack, payload_len) for TCP/IPv4."""
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = struct.unpack("<I", gh[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            endian = "<"
        else:
            endian = ">"
            magic = struct.unpack(">I", gh[:4])[0]
        nano = magic == 0xA1B23C4D
        linktype = struct.unpack(endian + "I", gh[20:24])[0]
        l2 = 14 if linktype == 1 else 0
        rec = struct.Struct(endian + "IIII")
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            ts, frac, incl, orig = rec.unpack(h)
            pkt = f.read(incl)
            t = ts + frac * (1e-9 if nano else 1e-6)
            if l2 and pkt[12:14] != b"\x08\x00":
                continue
            ip = pkt[l2:]
            if len(ip) < 20 or ip[9] != 6:
                continue
            ihl = (ip[0] & 0x0F) * 4
            tot = struct.unpack(">H", ip[2:4])[0]
            tcp = ip[ihl:]
            if len(tcp) < 20:
                continue
            sport, dport, seq, ack = struct.unpack(">HHII", tcp[:12])
            doff = (tcp[12] >> 4) * 4
            yield t, ip[12:16], sport, dport, seq, ack, tot - ihl - doff


def _flows(path: Path):
    """Per client port: sorted data (end_seq, t), retransmitted ends, ACKs."""
    data: dict[int, dict[int, float]] = {}
    retx: dict[int, set[int]] = {}
    acks: dict[int, list[tuple[float, int]]] = {}
    for t, _src, sport, dport, seq, ack, plen in read_pcap(path):
        if dport in DATA_PORTS and plen > 0:
            end = (seq + plen) & 0xFFFFFFFF
            d = data.setdefault(sport, {})
            if end in d:
                retx.setdefault(sport, set()).add(end)
            else:
                d[end] = t
        elif sport in DATA_PORTS:
            acks.setdefault(dport, []).append((t, ack))
    out = {}
    for port, d in data.items():
        ends = sorted(d)
        out[port] = (ends, [d[e] for e in ends], retx.get(port, set()), acks.get(port, []))
    return out


def ack_delays(path: Path) -> pd.DataFrame:
    """For each new cumulative ACK: time since the data it covers was seen.

    Matches the ACK to the captured segment holding its last acked byte (the
    first end_seq >= ack), as Linux's rate sample uses the most recently sent
    acked skb. Skips segments seen twice (retransmissions, Karn's rule).
    Sequence wraparound within one short run is not handled.
    """
    rows = []
    for port, (ends, times, retx, acks) in _flows(path).items():
        last = -1
        for t, a in acks:
            if a <= last:
                continue
            last = a
            i = bisect.bisect_left(ends, a)
            if i == len(ends) or ends[i] in retx:
                continue
            dt = t - times[i]
            if dt >= 0:
                rows.append((port, a, dt * 1e6))
    return pd.DataFrame(rows, columns=["port", "ack", "us"])


def decompose(run: Path) -> dict[str, float]:
    tx = ack_delays(run / "tx.pcap") if (run / "tx.pcap").exists() else pd.DataFrame()
    rx = ack_delays(run / "rx.pcap") if (run / "rx.pcap").exists() else pd.DataFrame()
    row: dict[str, float] = {"run": run.name}
    parts = {"tx_rtt": tx, "turnaround": rx}
    if len(tx) and len(rx):
        j = tx.merge(rx, on=["port", "ack"], suffixes=("_tx", "_rx"))
        j["us"] = j["us_tx"] - j["us_rx"]
        parts["wire_queue"] = j
        row["matched"] = len(j)
    tr = run / "trace.csv"
    if not tr.exists() and (run / "trace.csv.gz").exists():
        tr = run / "trace.csv.gz"
    if tr.exists():
        t = pd.read_csv(tr, usecols=["type", "rtt_us"])
        parts["module"] = t[(t["type"] == 0) & (t["rtt_us"] > 0)].rename(columns={"rtt_us": "us"})
    for name, df in parts.items():
        if len(df):
            for p, v in zip(PCTS, np.percentile(df["us"], PCTS)):
                row[f"{name}_p{p}"] = round(float(v), 1)
            row[f"{name}_n"] = len(df)
    return row


def main(argv: list[str]) -> int:
    rows = [decompose(Path(a)) for a in argv]
    df = pd.DataFrame(rows)
    cols = ["run"] + [f"{n}_p50" for n in ("module", "tx_rtt", "turnaround", "wire_queue")] \
        + [f"{n}_p99" for n in ("module", "tx_rtt", "turnaround", "wire_queue")]
    print(df[[c for c in cols if c in df]].to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
