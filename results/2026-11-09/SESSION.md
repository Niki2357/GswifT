# Session 2026-11-09: first evaluation run

## Environment

| | |
|---|---|
| Nodes | 2 x c6525-25g, CloudLab Utah (amd108 = node0 sender, amd111 = node1 receiver) |
| CPU / NIC | AMD EPYC 7302P 16-core; ConnectX-5 25 GbE, MTU 1500, HW timestamp filter `all` |
| OS / kernel | Ubuntu 22.04 image, upgraded to HWE kernel 6.8.0-138 (stock 5.15 cannot pace from BPF) |
| libbpf | 1.4.5 built from source (distro has 0.5) |
| Tuning | C-states disabled, coalescing off, quickack, slow_start_after_idle=0; no cpufreq governor exposed |
| Baseline | 64 B TCP RTT p1 40.7 / p50 42.2 / p99 55.3 / p99.9 80.7 µs; ping 1 ms interval avg 22 µs |
| Bottleneck | receiver ingress IFB, HTB 10 Gbit/s, FIFO 300 pkts (100 for incast), CE at 50 µs |

Full node details: `session/nodes.txt`. Summary tables and figures: `summary.md`, `tables/`, `figs/`.
Per-ACK `trace.csv` files are only on the local machine that ran the session (gitignored, ~1.2 GB).

## What ran

| Exp | Runs | Status |
|---|---|---|
| E0 noise floor | 3 loads | complete |
| E1 comparison | 5 CCs x {1, 8} flows | complete |
| E2 target sweep | 8 targets | complete |
| E3 incast | 4 variants x N=16..256 (100 rounds), N=512 (30 rounds) | complete |
| E4 spraying | swift, mswift, cubic x 5 q | complete but confounded (see below); mswift-k4 dropped for time |
| E5 diagnostic | swift, swift-nofrac, mswift at 8 flows, full tracing | complete |

Every configuration ran **once**.

## Outcomes

1. One flow: Swift 92.5% util, queue p99 36 µs, RPC p99 134 µs (best of the high-throughput CCs).
2. Software RTT grows with load with no network queue: p50 56 µs at 1 Gb/s to 340 µs at 16 Gb/s.
3. 8 flows, target 100 µs: Swift 22% / 53% / 35% utilization across three runs. It reacts to the host-delay tail (delay p99 161-182 µs). It needs a target of about 300 µs for 93%.
4. Incast: Swift beats Cubic/DCTCP from N=64 up. With a one-packet floor it takes 23 ms vs 223 ms at N=64. At p99 it is 1.6-2.8x better at N=128-512.
5. Fractional cwnd (min 0.01) made incast worse than a min of 1: 144x more drops at N=64.
6. E4 is confounded: TSO/GSO off caps the sender (Cubic 3.8 Gb/s at q=0), and Linux fast recovery fires 5k-200k times per run, so Swift's decreases bypass the median filter.

## Problems hit and fixed (all in the scripts now)

- Kernel 5.15 rejects BPF writes to `sk_pacing_rate`/`sk_pacing_status`: need kernel >= 6.0.
- Verifier 1M-instruction limit on the sqrt for flow scaling: compiled out of BPF (`-DSW_NO_FLOW_SCALING`).
- Sub-packet pacing ran 1.8x fast: the module halves the rate below cwnd 1 (now 0.89x). Root cause unconfirmed.
- Non-root iperf3 cannot pick algorithms outside `tcp_allowed_congestion_control`: scripts allow all registered.
- `sudo` ignores signals from its own process group: stop with `pkill -INT -x swiftctl`; liveness via `sudo kill -0`.
- Background processes held ssh sessions open: all background fds redirected.
- `tune.sh` aborted where there is no cpufreq governor, and setup hit SIGPIPE under pipefail: both fixed.
- `nstat -az` saved since-boot totals: now `nstat -z` per run. Earlier runs are reconstructed in `analysis/loaders.cumulative_to_delta`.
- Never `pkill -f <pattern>` from a command whose own text contains the pattern (it killed the run once).

## Open questions

- Why fractional cwnd hurts incast; why the pacing error is 1.8x.
- Cubic at 8 flows only 52% in E1; all CCs at N=32 incast take 11-16 ms vs a 1.7 ms bound.
- E4 measured spray fraction 0.30 at q=0.5 in Swift runs (0.46 for Cubic).
