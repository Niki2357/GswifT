# Session 2026-11-10: repeats, MSwift-focused E4, pacing fix

## Environment

| | |
|---|---|
| Nodes | 2 x xl170, CloudLab Utah (hp090 = node0 sender, hp093 = node1 receiver) |
| CPU / NIC | Intel Xeon E5-2640 v4 (10 cores, 20 threads); ConnectX-4 Lx 25 GbE, MTU 1500, HW timestamp filter `all` |
| OS / kernel | Ubuntu 20.04 image (the experiment came up with it), Ubuntu mainline kernel 6.8.0-060800 installed alongside 5.4 |
| Toolchain | clang 14 (apt.llvm.org), bpftool v7.4.0 release, libbpf 1.4.5, tc from iproute2 6.8.0 in /usr/local/sbin |
| Tuning | `tune.sh` applied; coalescing off, quickack, slow_start_after_idle=0. cpufreq governor reported `ondemand` |
| Baseline | 64 B TCP RTT p1 35.3 / p50 38.5 / p99 53.9 / p99.9 68.5 µs |
| Bottleneck | receiver ingress IFB, HTB 10 Gbit/s (E4: 2 Gbit/s), fq_codel flows 1 drop_batch 1, 300 pkts (100 for incast), CE at 50 µs |

**Different hardware from 2026-11-09 (c6525-25g, AMD EPYC, ConnectX-5).** Compare
numbers within a session, not across.

Full node details: `session/nodes.txt`. Run log: `session/run.log`. Tables and
figures: `summary.md`, `tables/` (`*_agg.csv` = median and min-max over
repeats), `figs/`. Per-ACK `trace.csv` files are local only (gitignored, about 6 GB).

## What ran (`scripts/run_session2.sh`, 11:04 to 12:37)

| Exp | Runs | Status |
|---|---|---|
| E0 noise floor | 3 loads, once | complete |
| E1 comparison | swift, cubic, dctcp x {1, 8} flows x 5 repeats | complete |
| E2 target sweep | targets 100, 200, 300 x 5 repeats | complete |
| E3 incast | swift, swift-nofrac, swift-pmin, swift-pcap, cubic x N = 64, 128 x 5 repeats | complete |
| E4 spraying, default loss recovery | swift, mswift, mswift-k4, cubic x 5 q x 3 repeats | complete |
| E4 spraying, REORDER_TOL=64 | same, with RACK off and dupthresh 64 | complete |

## Outcomes

1. E1/E2: at 8 flows Swift reached 91.5% (89.4-92.9) with target 100 µs and
   95.5% with 200 µs; Cubic 90.6%, DCTCP 95.1%. On 2026-11-09 Swift got
   22-53% and Cubic 52%, so the 8-flow under-utilization did not reproduce on
   this hardware. Swift had the lowest p99 queue at 8 flows (84 µs vs 119
   Cubic, 133 DCTCP).
2. E3: Swift with a one-packet floor finishes N=64 rounds in 14.9 ms median
   (Cubic 213 ms), reproducing 2026-11-09. At N=128 all variants are about
   220 ms.
3. E3: the fractional window still loses (p99 1.55 s). Pacing from min RTT
   plus a 1 ms gap cap cuts p99 to 0.28 s but not the median (206 ms). Most
   flows finish faster than with the floor (median FCT 12.7 vs 14.5 ms); about
   4 of 64 flows per round wait about 200 ms, without counted RTOs.
4. E4 default recovery: sender no longer CPU-limited (1.91 Gb/s at q=0).
   Linux fast recovery fires 70k-260k times per run and dominates.
5. E4 REORDER_TOL=64: fast recoveries drop to about 0. At q=0.25 Swift gets
   1.73 Gb/s, MSwift 1.90, MSwift k=4 1.91. At q=0.5 Swift 0.07, MSwift 0.05,
   MSwift k=4 1.61, Cubic 1.81.

## Problems hit and fixed

- Ubuntu 20.04 offers only kernel 5.15: installed mainline 6.8 with a
  one-shot `grub-reboot` first, so a failed boot would fall back to 5.4.
- Mainline modules are zstd-compressed and 20.04's kmod 27 cannot load them:
  decompressed all modules, `depmod`, rebuilt the initramfs before rebooting.
- Interface renamed to `ens1f1np1` on 6.8 (scripts find it by IP).
- node1's route to the Ubuntu mirror ran at about 70 KB/s: copied node0's apt
  cache over the experiment link.
- 20.04's tc (iproute2 5.5) cannot set fq_codel `drop_batch`: built tc from
  iproute2 v6.8.0. `setup_node.sh` now warns when tc lacks it.
- E4 `q_measured` used iptables skb counts (pre-segmentation, size depends on
  the CC). Now the netem/prio byte ratio; every variant gets the intended
  share. Slow-path packets come in short bursts (about 1.5-2.5 segments).
- `pkill -f` matched its own command again once (setup stop). Same rule as
  last time: match on something the killing command does not contain.

## Open questions

- What the 200 ms wait in fractional-window incast flows is (not RTO by
  `TcpExtTCPTimeouts`; about 1,150 tail-loss probes per run).
- Why 8-flow utilization differed so much between c6525 (Nov 9) and xl170.
- Governor `ondemand` on both nodes; tune.sh could not set `performance`.
