# GSwifT: Swift Congestion Control in the Linux Kernel

Ethan Kim, Gregory Park, Niki Hu, Xiaomeng Ye, Raymond Zhao
Georgia Tech, Fall 2026. Code and data: https://github.com/Niki2357/GswifT

> DRAFT (2026-11-20). Sections marked [pending] wait on the session-3 runs.
> Structure follows Ousterhout, "A Linux Kernel Implementation of the Homa
> Transport Protocol" (ATC 2021), as requested in the report-2 feedback.

## Abstract

[pending: written last, from the evaluation's headline numbers.]

## 1 Introduction

Swift [Kumar et al., SIGCOMM 2020] is the delay-based congestion control
Google runs in its datacenters. It keeps end-to-end delay near a target with
AIMD on the congestion window, splits delay into fabric and endpoint parts,
lets the window fall below one packet for large incasts, and scales its target
with path length and flow count. Google reports that it holds tail latency low
at high utilization. The published system, however, runs on Pony Express, an
OS-bypass transport inside the Snap networking stack, with NIC hardware
timestamps and fine-grained pacing. None of these is available to an
application on a stock Linux host.

This paper asks how much of Swift survives when it is implemented as an
ordinary Linux TCP congestion control, with the kernel's software RTT samples
as its only delay signal. The question has the same shape as the one
Ousterhout asked for Homa [ATC 2021]: a protocol evaluated in a specialized
setting, re-implemented inside the general-purpose kernel to see which
benefits hold and what the kernel costs. Our answer differs in where the cost
lands. Homa/Linux kept Homa's benefits and was limited by software overheads.
For Swift, software overheads become part of the control signal itself:
host delay is most of what a Linux sender measures, it grows with the
sender's own load, and the controller reacts to it as if it were congestion.

We make four contributions.

1. **An implementation.** GSwifT is Swift's Algorithm 1, MSwift's median
   filter and reordering tolerance [Gerstein et al., 2025], and fractional
   windows enforced by pacing, written as a BPF `struct_ops` congestion
   control that loads into unmodified Ubuntu kernels (Section 3).
2. **A list of what Linux forces on a delay-based controller** (Section 4):
   where the RTT sample comes from, how a sub-packet window must be expressed,
   why a lost lone packet costs 200 ms, how Linux's loss recovery overrides
   the controller under reordering, and what the BPF verifier and the 104-byte
   per-socket state rule out. Each is backed by a measurement.
3. **An evaluation by aspect** (Section 6): delay-signal fidelity,
   steady-state throughput and latency against the target, realistic RPC
   workloads, incast, and multipath spraying, against Cubic and DCTCP, on two
   CloudLab hardware types with repeated runs.
4. **Two pitfalls in common evaluation harnesses** that change Swift's
   results by up to an order of magnitude (Section 5.4), and the fixes.

## 2 Background: Swift and MSwift

[To write: one page summarizing the algorithm as the Homa paper summarizes
Homa, with a figure of the control loop.]

- Target-delay AIMD: if delay < target, `cwnd += ai * acked / cwnd`; else
  `cwnd *= max(1 - beta * (delay - target) / delay, 1 - max_mdf)`, at most
  once per RTT.
- Fabric and endpoint delay with separate windows; target scaled by hop
  count and by `1/sqrt(cwnd)`.
- `cwnd < 1`: one packet every `RTT / cwnd`, enforced by pacing.
- Retransmission timeouts reset the window after repeated loss.
- MSwift: with packet spraying, a fraction q of packets on a congested path
  makes Swift's throughput fall as `1/sqrt(q)`. The fix reacts to the median
  of the last `H = max(cwnd/2, 1)` samples and decreases only after k
  consecutive over-target samples.

## 3 GSwifT design

[To write from report 2 Section 4.2, reorganized: the core header shared with
unit tests; the mapping table from Swift mechanisms to Linux hooks; per-socket
state layout; telemetry; the loader and its parameters; the new options
(`--target-floor`, `--min-snd-cwnd`, `--rto-min`, `--pace-raw`).]

## 4 Implementation challenges

Organized as the Homa paper organizes its obstacles: what Linux forced, what
we did, and what it costs. Numbers come from Section 6 and the E9
measurements.

### 4.1 The delay signal is mostly host time

Linux gives a congestion control one RTT sample per ACK
(`rate_sample.rtt_us`), measured from when TCP handed the segment down to when
it processes the ACK. On an idle path the network accounts for about 3 µs of a
47 µs RTT (report 1, PTP measurement). Under load the sample grows with the
sender's own rate even with no queue in the network: median 63 µs at 1 Gb/s
and over 300 µs at line rate on one flow (E7). A two-ended packet capture
splits this: [pending: E7 table, sender stack / receiver turnaround / NIC and
interrupt paths per load].

### 4.2 Linux's window is an integer of at least one

Swift needs windows below one packet. GSwifT keeps a Q16 fixed-point window in
its own state, gives Linux `max(1, floor(cwnd))`, and enforces the fraction
with `sk_pacing_rate` and TCP's internal pacing, which BPF may set only on
kernels 6.0 and later. Pacing below one packet delivered 1.8x the requested
rate; [pending: E9 root cause and fidelity table].

### 4.3 A lost lone packet costs the minimum RTO

With one packet in flight, Linux's tail-loss probe waits `2 * srtt +
rto_min` (200 ms) before probing, to allow for a delayed ACK. A fractional
window always has one packet in flight, so every drop costs about 200 ms
(E6 packet capture, report 2). GSwifT can now lower `rto_min` per socket from
inside the module (`bpf_setsockopt(TCP_BPF_RTO_MIN)`) and keep two packets in
flight, sent as one two-segment burst per pacing gap, so that a single loss is
repaired by SACK. [pending: E3 results.]

### 4.4 Linux's loss recovery acts before the controller

Linux treats reordering as loss: sprayed packets trigger fast recovery,
which cuts the window regardless of the delay signal. MSwift's median filter
only matters once recovery tolerates reordering (RACK off, `tcp_reordering`
64); see Section 6.6.

### 4.5 The verifier and the per-socket budget

Flow-based target scaling needs `1/sqrt(cwnd)` per ACK; our branch-free
integer square root exceeded the verifier's one-million-instruction budget
and is compiled out. Per-socket state must fit in 104 bytes (`icsk_ca_priv`),
which caps MSwift's median window at 16  16-bit samples. [pending: E9 cost:
ns per `cong_control` call and cores per Gb/s versus Cubic.]

## 5 Methodology

[To write. All setup lives here, none in Section 6.]

### 5.1 Testbeds

Three sessions on CloudLab Utah: c6525-25g (AMD EPYC 7302P, ConnectX-5,
Nov 9 and Nov 20) and xl170 (Intel E5-2640 v4, ConnectX-4 Lx, Nov 10). Kernel
6.8 on all. Per-session unloaded RTT baselines.

### 5.2 Emulated bottleneck

Receiver ingress via IFB, HTB rate limit, single `fq_codel` FIFO with CoDel
disabled, `drop_batch 1`, CE marking at 50 µs for DCTCP. GRO is off while the
bottleneck is up so the buffer counts wire packets (Section 5.4).

### 5.3 Workloads

Bulk flows (iperf3, one process per flow), a 64-byte RPC probe, synchronized
incast, open-loop RPC workloads drawn from the DCTCP web-search distribution
and a short-RPC mix, and per-wire-packet spraying at the receiver.

### 5.4 Two harness pitfalls

1. **A single-threaded traffic generator couples flows.** iperf3 3.9 drives
   all `-P` streams from one thread with blocking writes. When one Swift flow
   drops to a window of 0.01 packets (one packet every ~30 ms), its full send
   buffer blocks the writes of every other stream. At a 100 µs target, no flow
   was sending in 77% of 1 ms intervals (8 flows, 2026-11-20). Running one
   iperf3 process per flow [pending: A/B numbers].
2. **An skb-counted buffer is not a packet-counted buffer.** With GRO on at
   the receiver, the bottleneck FIFO's 300-packet limit counted GRO
   aggregates; queued skbs averaged 5-55 KB in bulk runs, so the buffer held
   up to about 16 MB and loss-based Cubic rarely saw a drop. Incast runs were
   unaffected (1514 B per skb). GRO is now off at the bottleneck.

### 5.5 Repetition and statistics

Repeats interleaved across the session; medians with min-max ranges.

## 6 Evaluation

[pending. One subsection per aspect, results only:]

- 6.1 Basic latency and throughput (table: unloaded RTT, 1 and 8 flows).
- 6.2 Delay-signal fidelity (E0, E7).
- 6.3 Steady state against the target (E1/E2 block; floor-relative target).
- 6.4 RPC workloads: slowdown versus message size (E8).
- 6.5 Incast (E3 with module fixes).
- 6.6 Multipath spraying and MSwift (E4, per-packet, delay sweep).
- 6.7 Buffer occupancy.

## 7 Software overheads

[pending: E9 cost table and the per-stage RTT timeline from E7, analogous to
Homa/Linux Table 4 and Figure 7.]

## 8 Related work

[To write, positioning rather than listing: Swift and Falcon (hardware
endpoints, what we cannot have); MSwift (the multipath model we test on a
real stack); TIMELY (RTT-gradient CC with NIC timestamps); DCTCP and Cubic
(our baselines); BBR (model-based, also paced); Homa/Linux (the closest
methodological precedent: a datacenter transport re-implemented in Linux to
measure what the kernel costs); BPF congestion control (`bpf_dctcp`,
`bpf_cubic` selftests) and prior eBPF transport work; studies of host delay
and software timestamping (e.g. Report 1's PTP measurement).]

## 9 Conclusion

[pending]
