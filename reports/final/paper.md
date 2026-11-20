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

**Swift's control law.** Swift [Kumar et al., SIGCOMM 2020] compares each
RTT sample with a target delay. Below the target it grows the window
additively, by `ai / cwnd` per acknowledged packet (about `ai` packets per
RTT). Above the target it cuts multiplicatively, in proportion to how far
the delay overshoots:

    cwnd <- cwnd * max(1 - beta * (delay - target) / delay, 1 - max_mdf)

and it cuts at most once per RTT, so one burst of late ACKs causes one cut,
not many. A retransmission timeout also cuts, and repeated timeouts reset
the window to its minimum.

**Targets.** The target is a base value plus two scaling terms: one per
network hop on the path (so longer paths tolerate more queuing) and one
proportional to `1/sqrt(cwnd)` (flow-based scaling, so that when many flows
share a bottleneck the ones with small windows get a larger target and the
aggregate queue stays bounded while flows stay fair).

**Fabric and endpoint delay.** Swift's RTT is measured with NIC timestamps,
and the receiver reports how long each packet waited in its own host. Swift
runs two windows, one driven by fabric delay and one by endpoint delay, and
sends with the smaller. A busy receiver therefore slows its senders without
being mistaken for network congestion.

**Below one packet.** For incasts of thousands of flows even one packet per
flow per RTT overflows a switch buffer. Swift lets the window fall to a
small fraction of a packet and enforces it by pacing: one packet every
`RTT / cwnd`.

**Deployment context.** Swift runs in Pony Express, a transport in Google's
Snap user-space networking stack, with NIC hardware timestamps and a
software pacer polling at microsecond granularity. Google reports
throughput near line rate with tail RPC latency close to the base RTT.

**MSwift.** When packets of one flow are sprayed over many paths and a
fraction q of them crosses a congested path, every slow sample triggers a
Swift decrease. Gerstein, Silberstein and Keslassy [arXiv 2025] show that
Swift's throughput then falls roughly as `1/sqrt(q)`. MSwift reacts to the
median of the last `H = max(cwnd/2, 1)` samples instead of the latest one,
which ignores the slow minority until q approaches 1/2, and adds reordering
tolerance: decrease only after k consecutive over-target samples.

## 3 GSwifT design

GSwifT is a TCP congestion control written in BPF and attached through
`struct_ops`, the interface Linux exposes for congestion controls written as
BPF programs since kernel 5.6. It needs no kernel changes and loads into a
stock Ubuntu 22.04 kernel (6.8). A user-space loader, `swiftctl`, sets the
parameters, registers the algorithm under the name `swift`, and streams
telemetry; applications select it per socket with `TCP_CONGESTION` like any
built-in algorithm.

**One algorithm, two builds.** The control law lives in one C header of pure
functions over a per-socket state struct (`src/swift_core.h`). It obeys BPF's
rules (no floating point, no unbounded loops, no recursion), so the same file
compiles into the kernel module and into a user-space test binary that checks
it against hand-computed cases on every build (100,094 checks). Windows and
factors are Q16 fixed point.

**Mapping Swift onto Linux's hooks.**

| Swift mechanism | GSwifT |
|---|---|
| Delay sample | `rate_sample.rtt_us`, per ACK, in `cong_control` |
| Target | `base + hops * h`; per-destination hop count from a BPF map; optionally plus the flow's minimum RTT (`--target-floor`) |
| Additive increase | `cwnd += ai * acked / cwnd` (or `ai * acked` below one packet) |
| Multiplicative decrease | as in Section 2, at most once per smoothed RTT |
| Window below one packet | Q16 window in module state; Linux gets `max(m, floor(cwnd))` with `m` = 1, or 2 with `--min-snd-cwnd 2`; the fraction is enforced through `sk_pacing_rate` and TCP's internal pacing |
| Retransmission timeout | `cwnd_event(CA_EVENT_LOSS)`: decrease; reset to the minimum after three in a row |
| Fast recovery | `set_state(TCP_CA_Recovery)`: decrease by `max_mdf`, once per RTT |
| Minimum RTO | optionally set per socket at connection start (`--rto-min`, via `bpf_setsockopt`) |
| MSwift median, reordering tolerance | median of the last `min(cwnd/2, 16)` samples; decrease after k consecutive over-target samples |
| Fabric/endpoint split, flow scaling | not implemented (Sections 4.1, 4.5) |

**State.** 72 bytes per socket: the Q16 window, the time of the last
decrease, the last target and delay, a sampling counter, the timeout count,
the hop count, the MSwift ring (16 samples of 16 bits) and the flow's RTT
floor. Linux reserves 104 bytes.

**Telemetry.** Every Nth ACK, and every decrease, timeout and recovery,
emits a 64-byte record through a BPF ring buffer: time, RTT sample, delay
used, target, window, Linux's integer window, pacing rate, packets in
flight. Global counters (ACKs, samples, increases, decreases, holds,
timeouts, recoveries, ACKs below one packet) are read at exit.

**Defaults.** Base target 100 µs, `ai` = 1, `beta` = 0.8, `max_mdf` = 0.5,
minimum window 0.01 packets, initial window 10.

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
   was sending in 77% of 1 ms intervals (8 flows, 2026-11-20). With one
   iperf3 process per flow, interleaved with the single-process mode over
   three repeats (GRO on), Swift's utilization at a 100 µs target rose from
   7-12% to 18-23% and no interval was idle; at 150 and 200 µs both modes
   gave 87-96%, and Cubic and DCTCP were unaffected (79-81% and 92-95%).
   The generator explained half of the collapse at 100 µs; the rest is the
   host delay of Section 4.1.
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

**Delay-based datacenter congestion control with hardware support.**
TIMELY [Mittal et al., SIGCOMM 2015] reacts to the RTT gradient measured
with NIC timestamps for RDMA traffic. Swift [Kumar et al., SIGCOMM 2020]
switched to an absolute target delay, split fabric from endpoint delay, and
added sub-packet windows; Falcon [Singhvi et al., SIGCOMM 2025] moves a
two-window Swift into NIC hardware. All three take precise timestamps and
fine-grained pacing as given. We remove those assumptions and measure what
happens to the same control law when the delay signal is the kernel's
software RTT, and when pacing is Linux's internal pacer. Our central finding,
that host delay is most of the signal and grows with the sender's own load,
is the reason those systems rely on hardware.

**Multipath spraying.** MSwift [Gerstein et al., 2025] models Swift under
packet spraying and proposes the median filter; its evaluation is analytical
and simulated. We test the filter on a real stack, where Linux's own
reordering detection interacts with it (Section 6.6), with the spray choice
made per wire packet.

**Datacenter transports re-implemented in Linux.** Homa/Linux [Ousterhout,
ATC 2021] re-implemented a transport first evaluated in simulation and a
kernel-bypass system, to measure which benefits survive the kernel; it found
that software overheads, not network congestion, limit latency. Our study has
the same method and a related conclusion with a different consequence: for a
delay-based controller, software overheads do not only add latency, they
enter the control loop as false congestion. Unlike Homa, Swift needs no new
transport, so GSwifT keeps TCP and its API and replaces only the congestion
control.

**Congestion control in BPF.** Linux has accepted TCP congestion controls as
BPF `struct_ops` programs since kernel 5.6; the kernel's own self-tests port
DCTCP and Cubic this way. GSwifT uses the same interface for an algorithm
whose state and pacing needs go beyond those ports (sub-packet windows,
per-ACK median filtering), and documents where the interface constrains it
(Sections 4.2 and 4.5).

**Baselines.** Cubic [Ha et al., 2008] is Linux's default loss-based
algorithm. DCTCP [Alizadeh et al., SIGCOMM 2010] reacts to the fraction of
ECN-marked packets and is the standard datacenter baseline that, like Swift,
targets low queues; unlike Swift it needs switch marking, which our
bottleneck provides. Vegas [Brakmo and Peterson, 1995] is the classic
delay-based algorithm and BBR [Cardwell et al., 2016] a model-based one that
also paces; both appear only in our first session.

## 9 Conclusion

[pending]
