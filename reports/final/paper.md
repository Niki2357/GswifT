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
kernels 6.0 and later. On our first two kernels (Ubuntu 6.8.0-138 and
mainline 6.8.0) sub-packet pacing delivered about 1.8x the requested rate,
and the module halved its request to compensate. On Ubuntu 6.8.0-149 the
pacer is exact (0.994-1.000x of the requested rate for windows of 0.05-0.5
packets, E9), so the correction is now off by default (`--pace-halve`
enables it) and must be measured per kernel. With it on, every fractional
window paced at half its intended rate. [pending: E9 repeats, including an
intermittent mode at about 0.4x.]

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

All traffic arriving at the receiver is redirected through an IFB device,
rate-limited to 10 Gb/s by HTB with a 1 MB token bucket, and queued in a
single FIFO: `fq_codel` with one flow queue, CoDel disabled, `drop_batch 1`,
bounded at the byte equivalent of 300 packets (100 for incast). Packets
queued longer than 50 µs are CE-marked; only DCTCP negotiates ECN. A monitor
samples the FIFO backlog every 10 ms.

**What the emulator cannot do.** Two-ended captures show that the IFB and
HTB path itself adds delay that the FIFO backlog does not show: about 80 µs
when Swift holds the queue near empty at a 100 µs target, about 35 µs at
300 µs, against 4-6 µs of receiver turnaround without the bottleneck. With
a smaller token bucket (256-512 KB) HTB throttled often enough that this
hidden delay collapsed Swift under an RPC load it handled easily without
HTB. Targets below about 150 µs therefore measure the emulator, not Swift;
Section 6.3 tests them without a bottleneck and with a veth-based
alternative (E11). HTB occasionally passed up to 2-6% above its configured
rate (two of about 40 baseline runs).

### 5.3 Workloads

Bulk flows (iperf3, one process per flow), a 64-byte RPC probe, synchronized
incast, open-loop RPC workloads drawn from the DCTCP web-search distribution
and a short-RPC mix, and per-wire-packet spraying at the receiver.

### 5.4 Three harness pitfalls

Our first two sessions and the start of the third used a harness with three
flaws, each of which changed some result by up to an order of magnitude. We
found them on 2026-11-20 while explaining Swift's collapse at low targets;
all results in Section 6 use the corrected harness (`results/2026-11-20b`),
and the evidence for each flaw is in `results/2026-11-20`.

1. **A single-threaded traffic generator couples flows.** iperf3 3.9 drives
   all `-P` streams from one thread with blocking writes. When one Swift flow
   drops to a window of 0.01 packets (one packet every ~30 ms), its full send
   buffer blocks the writes of every other stream. At a 100 µs target, no flow
   was sending in 77% of 1 ms intervals (8 flows). With one iperf3 process
   per flow, interleaved with the single-process mode over three repeats,
   Swift's utilization at 100 µs rose from 7-12% to 18-23% and no interval
   was idle; at 150 and 200 µs both modes gave 87-96%, and Cubic and DCTCP
   were unaffected. The generator explained half of the collapse at 100 µs;
   the rest is the host delay of Section 4.1.
2. **An skb-counted buffer is not a packet-counted buffer.** The bottleneck
   runs on the receiver's ingress through an IFB device, after GRO. Its
   300-packet FIFO limit counted GRO aggregates; queued skbs averaged
   5-55 KB in bulk runs, so the buffer held up to about 16 MB. Turning GRO
   off fixes the accounting but caps the IFB path at 2.9 Gb/s (its
   per-packet cost), so we keep GRO on and bound the FIFO in bytes with
   fq_codel's `memory_limit` (2,180 bytes of skb memory per 1,514-byte
   packet, measured), which holds 300 packets at 10 Gb/s: Cubic now fills
   it to 319 µs median with 0.8% retransmissions. Incast runs were
   unaffected (1514 B per skb). Receiver-side spraying needs one decision
   per wire packet and keeps GRO off at 1 Gb/s.
3. **A global ECN setting changes the loss-based baseline.** We enabled
   `tcp_ecn=1` so DCTCP could negotiate ECN. Because applications choose
   the congestion control after `connect()`, DCTCP's own ECN request never
   reaches the SYN, so only the global setting enabled ECN, and it did so
   for every algorithm: our "Cubic" halved its window on the 50 µs CE marks
   meant for DCTCP and filled 80% of the link. The sender's `tcp_ecn` is now
   set per run (1 only for DCTCP); Cubic fills 96% with a full buffer, as a
   loss-based algorithm should. Swift was unaffected (its `cong_control`
   owns the window).

These flaws affect the Cubic and Swift numbers in our progress reports; the
qualitative incast and spraying findings are re-measured in Section 6.

### 5.5 Repetition and statistics

Repeats interleaved across the session; medians with min-max ranges.

## 6 Evaluation

[pending. One subsection per aspect, results only:]

- 6.1 Basic latency and throughput (table: unloaded RTT, 1 and 8 flows).
- 6.2 Delay-signal fidelity (E0, E7).
### 6.3 Steady state against the target

Eight bulk flows share the 10 Gb/s bottleneck; Swift's base target is swept
from 50 to 300 µs, interleaved with Cubic and DCTCP, five repeats.

| | Utilization | Queue p99 | RPC p99 |
|---|---|---|---|
| Cubic | 95.7% (95.7-95.7) | 391 µs | 519 µs |
| DCTCP | 96.9% (95.6-102) | 160 | 453 |
| Swift, 50-100 µs target | 16.5-17.5% | 1 | 128-181 |
| Swift, 150 µs | 91.8% (29.6-93.5) | 148 | 343 |
| Swift, 200 µs | 95.5% (95.3-95.6) | 174 | 455 |
| Swift, 300 µs | 95.6% (95.6-95.6) | 277 | 421 |
| Swift, flow min-RTT + 100 µs | 94.4% (92.8-95.3) | 142 | 513 |

**At 200 µs and above, Swift matches DCTCP's utilization** with a bounded
queue and no loss, and keeps the queue far below Cubic's full buffer.

**Below 150 µs the limit is the emulator, not Swift** (E11). Without a
bottleneck, so that the sender's 25 Gb/s NIC is the limit, the same eight
Swift flows run at 22.5-23.6 Gb/s at every target from 50 to 300 µs. Behind
the IFB bottleneck they fall to 1.7-1.8 Gb/s at 50-100 µs, and behind a veth
bottleneck (receiver in a network namespace, shaping on the veth egress) to
3.2, 4.7 and 7.7 Gb/s at 50, 75 and 100 µs; both reach 9.3-9.6 Gb/s from
150 µs. Section 5.2 attributes the difference to delay the software shaper
adds when it is saturated (about 80 µs for IFB at a 100 µs target). A
delay-controlled sender on Linux can hold RTT near 50 µs at line rate; a
timer-driven software shaper cannot emulate a switch at that scale.

**Near the floor, Swift is bistable, and per-flow target diversity removes
it.** At 150 µs one repeat in five collapsed (30-32%) in both the main block
and a later, independent block. With a per-flow random offset (each flow's
target is base + U[0, J), drawn at connection start) and the same mean
target, every repeat held 93-96% (base 130, J = 40; base 110, J = 80). At a
mean of 100 µs the offset raised utilization from 17-28% to a median of 80%
(38-93%). The floor-relative target behaves the same way, since flows'
minimum RTTs differ (per-flow targets 132-158 µs at +100 µs headroom). We
attribute this to de-synchronization: with identical targets, flows cross
the target together and cut together. A control rules out the simpler
explanation that the flows that draw the highest targets carry the load:
with every flow at 120 µs, the highest target any flow drew with base 80 and
J = 40, utilization was 19-28%, against 66-91% with the jittered targets
(five repeats each, interleaved). The fixed 150 µs target collapsed in one
of five repeats for the third time (28%), while base 130 with J = 40 held
91-95% and a fixed 170 µs held 96% in every repeat.

- 6.4 RPC workloads: slowdown versus message size (E8).
### 6.5 Incast

N connections on one sender each return 64 KB to the receiver at the same
moment, into a 100-packet buffer; 100 rounds per run, 3 repeats. Table 6.5
gives the 99th-percentile round completion time (median over repeats; the
repeats differ by under 10% in every cell).

| Configuration | N = 32 | 64 | 128 | 256 |
|---|---|---|---|---|
| Cubic | 210 ms | 216 | 632 | 3115 |
| DCTCP | 207 | 218 | 631 | 3115 |
| Swift, fractional window | 209 | 213 | 370 | 857 |
| Swift, one-packet floor | 11 | 23 | 222 | 614 |
| Swift, all fixes (two in flight, module `rto_min` 5 ms, min-RTT pacing) | 13 | 25 | 53 | 124 |
| Cubic, `rto_min` 5 ms | 9 | 21 | 52 | 121 |
| DCTCP, `rto_min` 5 ms | 8 | 20 | 46 | 102 |
| Swift, one-packet floor, `rto_min` 5 ms | 10 | 24 | 47 | 108 |

**Loss recovery, not the control law, decides incast completion time.**
With Linux's default 200 ms minimum RTO, every configuration that loses a
packet in most rounds finishes near a multiple of 200 ms: Cubic and DCTCP
at every fan-in, Swift with a one-packet floor from N = 128. Lowering the
minimum RTO to 5 ms brings all algorithms within 20% of each other
(102-124 ms at N = 256); DCTCP is fastest.

**Fractional windows need both lone-packet fixes.** Swift's sub-packet
window as published is the slowest Swift variant until N = 256 because each
dropped lone packet waits for the tail-loss probe (Section 4.3). Keeping two
packets in flight without a lower minimum RTO helps at N = 32 (26 ms) and
a little at N = 128, and is worse than the plain window at N = 256
(1508 ms); a lower
minimum RTO alone cuts N = 64 to 47 ms. Together with min-RTT pacing they
bring the fractional window to 124 ms at N = 256, 5x faster than the
one-packet floor at the default RTO, and level with Cubic and DCTCP at a
5 ms RTO.

**Swift's lasting advantage is loss avoidance.** At N = 64 Swift with a
one-packet floor retransmits 0.12% of segments, against 15.8% for Cubic
and 19.4% for DCTCP (default RTO); at N = 256 it is 2.2% against 21.5% and
26.7%. Fewer losses do not translate into faster rounds once recovery is
cheap, but they matter wherever retransmissions cost bandwidth or CPU.

**Distance from the ideal.** Draining N x 64 KB at 10 Gb/s takes 3.4 ms at
N = 64 and 13.4 ms at N = 256. The best configurations' median round times
are 13-23 ms and 60-110 ms, 4-8x the ideal. [pending: where the gap goes,
from per-flow FCTs and the queue trace.]

### 6.6 Multipath spraying and MSwift

The receiver's bottleneck (1 Gb/s, GRO off) sends each data packet, chosen
independently with probability q, through a slow path that adds 300 or
1000 µs; the measured share matched q within 0.02 in every run. Linux's
loss recovery is made reordering-tolerant (RACK off, `tcp_reordering` 64);
without it, every algorithm's window is cut by fast recovery on reordering
and the delay signal never matters (report 2). Base target 100 µs, two
repeats (which agree within 0.02 Gb/s).

| Flows | Slow path | Variant | q = 0.1 | 0.25 | 0.35 | 0.5 |
|---|---|---|---|---|---|---|
| 1 | +300 µs | Swift | 0.68 Gb/s | 0.24 | 0.13 | 0.03 |
| 1 | +300 µs | MSwift (median) | 0.95 | 0.58 | 0.16 | 0.03 |
| 1 | +300 µs | MSwift, k = 4 | 0.95 | 0.95 | 0.54 | 0.18 |
| 1 | +1000 µs | Swift | 0.45 | 0.13 | 0.05 | 0.01 |
| 1 | +1000 µs | MSwift | 0.95 | 0.21 | 0.06 | 0.01 |
| 1 | +1000 µs | MSwift, k = 4 | 0.95 | 0.62 | 0.19 | 0.05 |
| 2 | +300 µs | Swift | 0.95 | 0.47 | 0.24 | 0.06 |
| 2 | +300 µs | MSwift | 0.95 | 0.91 | 0.33 | 0.06 |
| 2 | +300 µs | MSwift, k = 4 | 0.95 | 0.95 | 0.95 | 0.35 |
| 8 | +300 µs | Swift | 0.95 | 0.95 | 0.84 | 0.23 |
| 8 | +300 µs | MSwift | 0.95 | 0.95 | 0.95 | 0.25 |
| 8 | +300 µs | MSwift, k = 4 | 0.95 | 0.95 | 0.95 | 0.95 |

With q = 0, every variant delivers 0.95 Gb/s; Cubic delivers 0.95-0.96 at
every q, at the cost of 700-1,050 fast recoveries per run.

**Swift collapses faster than the model predicts.** MSwift's analysis has
Swift's throughput falling as `1/sqrt(q)`. Here, once the decline starts it
is much steeper: for one flow at +300 µs throughput falls 5x from q = 0.1 to
0.35 (a log-log slope of about -1.3), and for two flows 2x from 0.25 to 0.35
(slope about -2). With 8 flows each window is about one packet and the
decline starts only at q = 0.35.

**The median helps only as far as its window reaches.** MSwift uses the
median of the last `max(cwnd/2, 1)` samples, capped at 16. At 1 Gb/s one
flow's window is about nine packets, so the median spans four samples and
holds full throughput to q = 0.1 but not 0.25; with two flows (median of
two or three) it holds to 0.25. With eight flows the window is about one
packet, the median degenerates to the latest sample, and MSwift behaves like
Swift.

**Reordering tolerance carries most of the benefit.** Decreasing only after
four consecutive over-target samples holds full throughput to q = 0.25 (one
flow), 0.35 (two flows) and 0.5 (eight flows, +300 µs). It needs no window
of samples, so it works where the median cannot.

- 6.7 Buffer occupancy.

## 7 Software overheads

Homa/Linux found that software overheads, not network congestion, limit a
kernel transport. For a delay-based controller the same overheads matter
twice: they cost CPU, and they appear in the delay signal.

**Where a Linux RTT sample's time goes** (E7). We captured headers on both
hosts during single-flow transfers with no bottleneck and Swift's target set
out of reach, so the window never shrinks, and split each RTT sample by
matching ACK numbers within each host's capture (no clock synchronization
needed). Table 7.1 gives medians.

| Load (one flow, no bottleneck) | Module RTT | Sender TCP + qdisc | Receiver TCP turnaround | NIC, driver and interrupt paths (both hosts) + wire |
|---|---|---|---|---|
| 1 Gb/s | 62 µs | 3 µs | 6 µs | 52 µs |
| 3 Gb/s | 96 | 3 | 4 | 88 |
| 6 Gb/s | 138 | 11 | 5 | 119 |
| unlimited | 346 | 21 | 4 | 321 |

The receiver's TCP stack answers in 4-6 µs at every load, and the sender's
stack and qdisc add little until line rate. Almost all of the sample, and all
of its growth with load, sits below TCP: in the NIC queues, drivers,
interrupt handling and GRO of both hosts, which a TCP congestion control can
neither see nor separate. This is the delay Swift's NIC timestamps exclude
and its endpoint/fabric split attributes; GSwifT has neither.

The growth is driven by how much data the sender keeps in flight, not by the
rate itself: with a 50 µs or 100 µs target and no bottleneck, eight Swift
flows ran at 23 Gb/s on the 25 Gb/s link (validation runs; E11 repeats
this), because a window bounded
by the target also bounds the queues in the host. The large delays in
Table 7.1 come from a window that never shrinks.

**CPU cost** (E9, eight flows through the 10 Gb/s bottleneck, all at 95.6%
utilization).

| | Sender cores | Receiver cores | BPF time per `cong_control` call |
|---|---|---|---|
| Cubic | 0.33 | 3.5 | (built-in) |
| DCTCP | 0.83 | 5.3 | (built-in) |
| Swift, telemetry off | 0.79 | 4.2 | 131 ns |
| Swift, one record per 16 ACKs | 0.88 | 4.2 | 853 ns |
| Swift, one record per ACK | 1.05 | 4.1 | 2,345 ns |
| MSwift (median of 16) | 0.69 | 4.2 | 844 ns |

The control law itself is cheap: 131 ns per ACK, under 0.02 cores at the
roughly 125,000 calls per second of these runs. Telemetry through the ring buffer costs up to 18x that, so all
evaluation runs sample one ACK in 4 or 16. MSwift's median over 16 samples
is quadratic in the window and costs 6x Swift's update, still small. Swift
and DCTCP each use about 2.5x Cubic's sender CPU at the same throughput;
both keep windows and queues small, which plausibly means smaller
transmissions per send, though we did not measure segment sizes.

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
