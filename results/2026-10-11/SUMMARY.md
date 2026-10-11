# Session 2026-10-11

| | |
|---|---|
| Nodes | node0 = clnode315 (10.10.1.1), node1 = clnode308 (10.10.1.2) |
| Node type | r6525, Clemson |
| Kernel | 6.8.0-138-generic |
| Interface | `enp129s0f0np0`, ConnectX-6 Dx, MTU 9000 |
| tune.sh | applied on both nodes (see `*/s1011/tune-log.txt`, `verify/*/governor.txt`, `coalescing.txt`) |
| Unloaded RTT | ICMP 1 s interval: min 64, avg 134 µs. ICMP 1 ms interval: min 23, avg 26 µs |

Note: at session start the governor was already `performance` and all C-states
were already disabled (set earlier this boot). Only interrupt coalescing was
still at default. `ping-cpu-tuned-coalesce-default.txt` (min 50, avg 127 µs) is
that state, not a true untuned baseline.

## 1. Hardware timestamps on TCP data (not just PTP)

`hwstamp_ctl -r 1` (HWTSTAMP_FILTER_ALL) accepted on both nodes.
`scripts/hwts_pingpong.c`: TCP ping-pong with NIC TX and RX timestamps on both
ends. 20,000 round trips per size, **0 missing hardware timestamps** at any size.

Each component is a difference of timestamps from a single NIC clock, so no
cross-node clock sync is needed.

| 64 B messages (µs) | p1 | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Application RTT | 63.0 | 71.8 | 98.5 | 107.6 |
| NIC-to-NIC RTT (client) | 41.5 | 44.1 | 59.1 | 69.6 |
| Server residence (NIC rx to NIC tx) | 39.0 | 41.6 | 56.5 | 66.4 |
| **Network (wire + switch, both ways)** | **2.459** | **2.471** | **2.493** | **2.499** |
| Client host (app RTT minus NIC RTT) | 20.7 | 25.3 | 50.3 | 58.6 |

At 64 B the network is 3.4% of the median application RTT. At 4 KB, network
p50 = 2.99 µs, still within ±30 ns p1 to p99.9.

At 64 KB the decomposition breaks (network p1 = -29.6 µs). The program pairs the
TX timestamp of the request's *last* byte with the RX timestamp of the reply's
*first* segment, which is wrong once TSO/GRO split or merge the message. Swift
needs per-packet timestamps; with Linux segmentation offloads that needs care.

## 2. struct_ops congestion control with cong_control and pacing

`scripts/cc_probe.bpf.c`, built with clang 18, registered with
`bpftool struct_ops register`. The verifier accepts `cong_control` and writes
to `snd_cwnd`, `sk_pacing_rate`, `sk_pacing_status`.

| Test | Result |
|---|---|
| cwnd 200, pace 2 Gbps | **2.00 Gbps**, 0 retransmits. 39,913 `cong_control` calls, each with an RTT sample (min 34, mean 44, max 3358 µs) |
| cwnd 1, pace 100 Mbps, receiver default | **2.3 Mbps**: about one packet per 31 ms, i.e. receiver delayed-ACK timer |
| cwnd 1, pace 100 Mbps, receiver route `quickack 1` | **100 Mbps** exactly |

Sub-packet-per-RTT pacing works, but only with quickack on the receiver.

iperf3 needs `sudo` for a BPF algorithm unless it is added to
`net.ipv4.tcp_allowed_congestion_control`.

## 3. Bug found

`tune.sh` ran `modprobe tcp_dctcp tcp_bbr tcp_vegas`, which loads only
`tcp_dctcp` and passes the other two as parameters
(`dmesg: tcp_dctcp: unknown parameter 'tcp_bbr' ignored`). Fixed with `-a`.

## Caveats

- phc2sys (PHC disciplined to CLOCK_REALTIME) held within ~1 µs most of the
  session but had a ~24 µs excursion near t=4242 s, probably during the clang
  install. Only the `client_rx_stack_ns` column depends on it.
- The 2.47 µs network RTT is somewhat below 2 x the PTP path delay from the
  previous session (2.68 µs). Different packet sizes and PTP's filtering likely
  account for it. Same order of magnitude either way.
