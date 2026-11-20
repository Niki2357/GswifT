#!/usr/bin/env python3
"""Merge per-flow iperf3 -J outputs into one document shaped like a single
`iperf3 -P N` run (end.streams, end.sum_received, end.sum_sent), so the
analysis loaders read either.

    merge_iperf.py OUT.json flow0.json flow1.json ...
"""
import json
import sys


def merge(docs: list[dict]) -> dict:
    streams, recv_bps, sent_bps, retx, recv_bytes = [], 0.0, 0.0, 0, 0
    for d in docs:
        end = d["end"]
        streams.extend(end.get("streams", []))
        recv_bps += float(end["sum_received"]["bits_per_second"])
        recv_bytes += int(end["sum_received"].get("bytes", 0))
        sent_bps += float(end["sum_sent"]["bits_per_second"])
        retx += int(end["sum_sent"].get("retransmits", 0))
    return {"start": docs[0].get("start", {}) if docs else {},
            "end": {"streams": streams,
                    "sum_received": {"bits_per_second": recv_bps, "bytes": recv_bytes},
                    "sum_sent": {"bits_per_second": sent_bps, "retransmits": retx}},
            "merged_from": len(docs)}


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    docs = []
    for p in argv[2:]:
        try:
            with open(p) as f:
                docs.append(json.load(f))
        except (OSError, ValueError) as e:
            print(f"merge_iperf: skipping {p}: {e}", file=sys.stderr)
    good = [d for d in docs if "end" in d and "sum_received" in d.get("end", {})]
    if not good:
        return 1
    with open(argv[1], "w") as f:
        json.dump(merge(good), f)
    return 0 if len(good) == len(argv) - 2 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
