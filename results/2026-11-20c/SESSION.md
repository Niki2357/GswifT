# Session 2026-11-20 (session 3): corrected harness, module fixes, all aspects

Three result folders come from this session:

| Folder | Contents | Use |
|---|---|---|
| `2026-11-20` | Phase 1 on the old harness (E0, interleaved E1/E2 block, E3), the iperf3 A/B, first E7 and E9 | evidence for the harness flaws only |
| `2026-11-20b` | E7 decomposition and E9 pacing/cost on the corrected harness (256 KB HTB bucket); later steps there were superseded | E7, E9 |
| `2026-11-20c` | everything else on the final harness (1 MB HTB bucket) | all other results |

## Environment

| | |
|---|---|
| Nodes | 2 x c6525-25g, CloudLab Utah (amd128 = node0 sender, amd155 = node1 receiver) |
| CPU / NIC | AMD EPYC 7302P 16-core (32 threads); ConnectX-5 25 GbE, MTU 1500 |
| OS / kernel | Ubuntu 22.04 image, HWE kernel 6.8.0-149 (apt) |
| Tuning | `tune.sh` applied; governor `performance` on node0; coalescing off; quickack; slow_start_after_idle=0 |
| Baseline | 64 B TCP RTT p1 37.0 / p50 38.3 / p99 51.3 / p99.9 58.4 µs (2026-11-20c/session) |

Same node type as 2026-11-09, so those numbers can be compared with care
(different kernel build and harness).

## Final harness (all of 2026-11-20c)

- One iperf3 process per bulk flow (iperf3 3.9 is single-threaded; one slow
  Swift stream stalled the others).
- Bottleneck: receiver ingress -> IFB -> HTB 10 Gb/s, 1 MB bucket ->
  fq_codel single FIFO, byte limit `memory_limit` = 300 x 2180 B (100 for
  incast), CE at 50 µs. GRO stays on. Receiver-side spraying turns GRO off at
  1 Gb/s.
- ECN only for DCTCP (sender `tcp_ecn` set per run).
- Goodput = received bytes / sender duration.
- Alternative bottleneck for E11 and some E8 runs: receiver apps in netns
  `rx` behind veth, HTB + FIFO on the veth egress (`scripts/rxns.sh`,
  `scripts/use_veth.sh`).
- Module: sub-packet pacing correction off by default (`--pace-halve` to
  enable), new options `--target-floor`, `--min-snd-cwnd`, `--rto-min`,
  `--target-jitter`, flow scaling by lookup table (`--fs-range`).

## What ran (2026-11-20c)

| Exp | Runs | Notes |
|---|---|---|
| Block (E1/E2) | Swift 50-300 µs, floor+60/100, Cubic, DCTCP x 5 repeats | |
| E3 incast | 8 variants x N = 32-256 x 3; plus 4 variants at rto_min 5 ms x 3 | |
| E8 RPC workloads | rpcmix / websearch x 50 / 80% x swift, swift-t200, cubic, dctcp x 3 (IFB) | Swift at 100 µs fails (emulator floor) |
| E4 spraying | 8 flows: q 0-0.5 x +100/300/1000 µs x 4 variants x 2, reorder-tolerant; default recovery at +300 µs | |
| E4 larger windows | 1 and 2 flows, +300/1000 µs, 3 variants x 2 | the median filter needs cwnd >= 4 |
| E10 host knobs | 6 knobs x 3 loads | |
| E11 emulator fidelity | Swift 50-300 µs x {no bottleneck, IFB, veth} x 3 | |
| Jitter blocks | fixed vs per-flow-jittered targets, two blocks x 5 | plus a fixed-max control |
| E8 on veth | swift-t200/t300, cubic, dctcp; flow scaling; burst sizes | 80% load investigation |

## Problems found and fixed during the session

1. iperf3 3.9 single-threaded: coupled flows (77% of 1 ms intervals idle at
   a 100 µs target).
2. Bottleneck FIFO counted GRO aggregates (300 "packets" = up to 16 MB).
3. GRO off caps IFB at 2.9 Gb/s.
4. Global `tcp_ecn=1` made Cubic react to DCTCP marks.
5. Sub-packet pacing correction (halving) wrong on this kernel: pacing is
   exact (0.94-1.04x over 45 runs).
6. iperf3 receiver-side duration sometimes inflated (18 s for 8 s), which
   under-reported goodput 2.2x.
7. HTB with a 256-512 KB bucket adds hidden delay under load; 1 MB fixes the
   RPC collapse at 50% load.
8. The IFB/HTB path adds 35-80 µs that the FIFO backlog does not show
   (two-ended capture); targets below ~150 µs measure the emulator.
9. Leaked queue monitors (aborted runs) kept running on node1 from about
   13:30 to 19:40, inflating qmon.csv files (duplicate samples, NUL holes) and
   adding some receiver CPU load; fixed at 19:40, so E11, the jitter blocks,
   the E4 window runs and the veth E8 runs are clean.
10. node0 disk filled (traces); traces and qmon files are now gzipped on the
    node.
11. My own mistakes: two failed attempts to stop runs (`pkill -x` on a script
    name) left a queue running alongside tests for about 20 minutes (that data
    was deleted); a qmon kill that matched its own shell aborted one batch.

## Raw data

Traces (`trace.csv.gz`), queue samples, probe and RPC logs, per-flow iperf
files and captures are not in git (size); they are on the local machine in
`results/2026-11-20*` and in the session archive. Tables and figures are
committed.
