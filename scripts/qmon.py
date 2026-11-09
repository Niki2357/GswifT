#!/usr/bin/env python3
"""Sample a qdisc's backlog and drop/mark counters to CSV.

    sudo python3 qmon.py ifb0 out.csv [interval_ms]

Reads `tc -s -j qdisc show dev DEV` and records, for the leaf qdisc (the one
with the largest backlog field set, i.e. fq_codel under HTB), the backlog in
bytes and packets plus cumulative drops and CE marks. Runs until killed.
"""

import csv
import json
import signal
import subprocess
import sys
import time


def leaf_stats(dev):
    raw = subprocess.run(["tc", "-s", "-j", "qdisc", "show", "dev", dev],
                         capture_output=True, text=True, check=True).stdout
    qdiscs = json.loads(raw)
    leaves = [q for q in qdiscs if q.get("kind") not in ("htb", "ingress")]
    q = leaves[0] if leaves else qdiscs[0]
    return {
        "backlog_bytes": q.get("backlog", 0),
        "backlog_pkts": q.get("qlen", 0),
        "drops": q.get("drops", 0),
        "ce_marks": q.get("ce_mark", q.get("ecn_mark", 0)),
        "sent_bytes": q.get("bytes", 0),
    }


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    dev, path = sys.argv[1], sys.argv[2]
    interval = float(sys.argv[3]) / 1000 if len(sys.argv) > 3 else 0.01

    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    fields = ["ts", "backlog_bytes", "backlog_pkts", "drops", "ce_marks", "sent_bytes"]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        while running:
            try:
                row = leaf_stats(dev)
            except (subprocess.CalledProcessError, json.JSONDecodeError, IndexError) as e:
                print(f"qmon: {e}", file=sys.stderr)
                time.sleep(interval)
                continue
            row["ts"] = f"{time.time():.4f}"
            w.writerow(row)
            time.sleep(interval)


if __name__ == "__main__":
    main()
