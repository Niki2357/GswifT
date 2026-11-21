# GSwifT: Swift Congestion Control in the Linux Kernel

Ethan Kim, Gregory Park, Niki Hu, Xiaomeng Ye, Raymond Zhao
Georgia Tech, Fall 2026. Code and data: https://github.com/Niki2357/GswifT

> DRAFT (2026-11-21). Structure follows Ousterhout, "A Linux Kernel
> Implementation of the Homa Transport Protocol" (ATC 2021), as requested in
> the report-2 feedback. Numbers come from `results/2026-11-20b` (E7, E9) and
> `results/2026-11-20c` (everything else) unless stated.

## Abstract

Swift is the delay-based congestion control behind Google's datacenter RPC
stack. It relies on NIC timestamps, an OS-bypass transport and a microsecond
pacer, none of which a stock Linux host has. We implement Swift, with
MSwift's multipath extensions, as a BPF congestion control for unmodified
Linux TCP and measure what survives. The control law itself is cheap (131 ns
per ACK) and, with a target of 200 µs, matches DCTCP's utilization through a
10 Gb/s bottleneck (95.5% against 96.9%) with no loss and a bounded queue.
But most of what a Linux sender measures is host time: a two-ended capture
shows that the delay that grows with load sits in the NIC, driver and
interrupt paths of both hosts, below TCP, where no standard setting removes
it. Making sub-packet windows work for incast required three Linux-specific
fixes; with them Swift matches Cubic and DCTCP once all use a short minimum
RTO, and it retransmits 10-100x fewer segments. MSwift's median filter helps
only when windows hold several packets, while its reordering tolerance works
at any window. Near its delay floor Swift is bistable; giving each flow a
slightly different target removes the instability. Under open-loop RPC load
Swift matches DCTCP at 50% but collapses at 80%, with RTT inflated outside
the network queue. Along the way we found that common emulation tools
(single-threaded traffic generators, skb-counted queues, a global ECN
setting, timer-driven shapers) can each change a delay-based controller's
results by an order of magnitude.

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
host delay is most of what a Linux sender measures, and the controller
cannot tell it apart from congestion.

We make five contributions.

1. **An implementation.** GSwifT is Swift's Algorithm 1 with flow-based
   target scaling, MSwift's median filter and reordering tolerance
   [Gerstein et al., 2025], and fractional windows enforced by pacing,
   written as a BPF `struct_ops` congestion control that loads into
   unmodified Ubuntu kernels (Section 3).
2. **What Linux forces on a delay-based controller** (Section 4): where the
   RTT sample comes from, how a sub-packet window must be expressed and how
   accurately Linux paces it, why a lost lone packet costs 200 ms and how to
   avoid it from inside the module, how Linux's loss recovery overrides the
   controller under reordering, and how to fit flow scaling into the
   verifier's budget. Each is backed by a measurement.
3. **An evaluation by aspect** (Section 6): delay-signal fidelity,
   steady-state throughput and latency against the target, open-loop RPC
   workloads, incast, and multipath spraying, against Cubic and DCTCP, with
   repeated runs.
4. **Per-flow target diversity.** Near its delay floor Swift is bistable;
   drawing each flow's target from a small range removes the instability
   (Section 6.3).
5. **Methodology lessons** (Sections 5.4, 5.2): four flaws in common
   emulation setups that each change Swift's results by up to an order of
   magnitude, and a measurement of how much delay a software bottleneck adds
   by itself.

## 2 Background: Swift and MSwift

**Swift's control law.** Swift compares each RTT sample with a target delay.
Below the target it grows the window additively, by `ai / cwnd` per
acknowledged packet (about `ai` packets per RTT). Above the target it cuts
multiplicatively, in proportion to how far the delay overshoots:

    cwnd <- cwnd * max(1 - beta * (delay - target) / delay, 1 - max_mdf)

and it cuts at most once per RTT, so one burst of late ACKs causes one cut,
not many. A retransmission timeout also cuts, and repeated timeouts reset
the window to its minimum.

**Targets.** The target is a base value plus two scaling terms: one per
network hop on the path, so longer paths tolerate more queuing, and one
proportional to `1/sqrt(cwnd)` (flow-based scaling), so that when many flows
share a bottleneck the ones with small windows get a larger target and the
aggregate queue stays bounded while flows stay fair.

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
Swift decrease. Gerstein, Silberstein and Keslassy show that Swift's
throughput then falls roughly as `1/sqrt(q)`. MSwift reacts to the median of
the last `H = max(cwnd/2, 1)` samples instead of the latest one, which
ignores the slow minority until q approaches 1/2, and adds reordering
tolerance: decrease only after k consecutive over-target samples.

## 3 GSwifT design

GSwifT is a TCP congestion control written in BPF and attached through
`struct_ops`, the interface Linux has offered for congestion controls written
as BPF programs since kernel 5.6. It needs no kernel changes and loads into a
stock Ubuntu 22.04 kernel (6.8). A user-space loader, `swiftctl`, sets the
parameters, registers the algorithm under the name `swift`, and streams
telemetry; applications select it per socket with `TCP_CONGESTION` like any
built-in algorithm.

**One algorithm, two builds.** The control law lives in one C header of pure
functions over a per-socket state struct (`src/swift_core.h`). It obeys BPF's
rules (no floating point, no unbounded loops, no recursion), so the same file
compiles into the kernel module and into a user-space test binary that checks
it against hand-computed cases on every build (101,433 checks). Windows and
factors are Q16 fixed point.

**Mapping Swift onto Linux's hooks.**

| Swift mechanism | GSwifT |
|---|---|
| Delay sample | `rate_sample.rtt_us`, per ACK, in `cong_control` |
| Target | `base + hops * h + fs(cwnd)`; per-destination hop count from a BPF map; options: plus the flow's minimum RTT (`--target-floor`), plus a per-flow random offset (`--target-jitter`) |
| Flow scaling `fs(cwnd)` | lookup table filled by the loader, read with one bounded array access (Section 4.5) |
| Additive increase | `cwnd += ai * acked / cwnd` (or `ai * acked` below one packet) |
| Multiplicative decrease | as in Section 2, at most once per smoothed RTT |
| Window below one packet | Q16 window in module state; Linux gets `max(m, floor(cwnd))`, `m` = 1 by default (`--min-snd-cwnd`); below `m` the rate is set through `sk_pacing_rate` and TCP's internal pacing |
| Retransmission timeout | `cwnd_event(CA_EVENT_LOSS)`: decrease; reset to the minimum after three in a row |
| Fast recovery | `set_state(TCP_CA_Recovery)`: decrease by `max_mdf`, once per RTT |
| Minimum RTO | optionally set per socket at connection start (`--rto-min`, via `bpf_setsockopt`) |
| MSwift median, reordering tolerance | median of the last `min(cwnd/2, 16)` samples; decrease after k consecutive over-target samples |
| Fabric/endpoint split | not implemented (Section 9) |

**State.** 72 bytes per socket: the Q16 window, the time of the last
decrease, the last target and delay, a sampling counter, the timeout count,
the hop count, the per-flow target offset, the MSwift ring (16 samples of 16
bits) and the flow's RTT floor. Linux reserves 104 bytes.

**Telemetry.** Every Nth ACK, and every decrease, timeout and recovery,
emits a 64-byte record through a BPF ring buffer: time, RTT sample, delay
used, target, window, Linux's integer window, pacing rate, packets in
flight. Global counters are read at exit.

**Defaults.** Base target 100 µs, `ai` = 1, `beta` = 0.8, `max_mdf` = 0.5,
minimum window 0.01 packets, initial window 10, flow scaling off.

## 4 Implementation challenges

As the Homa paper does for its obstacles, we describe what Linux forced, what
we did, and what it costs.

### 4.1 The delay signal is mostly host time

Linux gives a congestion control one RTT sample per ACK
(`rate_sample.rtt_us`), measured from when TCP handed the segment down to
when it processes the ACK. On an idle path the network accounts for about
3 µs of a 47 µs RTT (report 1, PTP measurement). Under load the sample grows
with the sender's own load even when no network queue exists: from 62 µs at
1 Gb/s to 346 µs for an unlimited flow (Section 7). Most of that growth is in
the NIC, driver and interrupt paths of both hosts, which a TCP congestion
control can neither observe nor separate from network queuing. Swift avoids
this with NIC timestamps and the endpoint/fabric split; GSwifT has neither.

### 4.2 Linux's window is an integer, and pacing must make up the rest

Linux's congestion window is an integer of at least one. GSwifT keeps a Q16
window in its own state, gives Linux `max(1, floor(cwnd))`, and enforces the
fraction with `sk_pacing_rate` and TCP's internal pacing, which a BPF
congestion control may set only on kernels 6.0 and later.

How faithfully Linux paces turned out to depend on the kernel build. On our
first two kernels (Ubuntu 6.8.0-138 and mainline 6.8.0) sub-packet pacing
delivered about 1.8x the requested rate, and the module halved its request
to compensate. On Ubuntu 6.8.0-149 the pacer is exact: with the window pinned
at 0.05 to 0.9 packets, 45 runs delivered 0.94 to 1.04 times the rate the
module set (E9). The halving now made every fractional window send at half
its intended rate, so it is off by default and available as `--pace-halve`.
Pacing accuracy has to be measured on each kernel.

### 4.3 A lost lone packet costs the minimum RTO

With one packet in flight, Linux's tail-loss probe waits `2 * srtt +
rto_min` (200 ms) before probing, to allow for a delayed ACK. A fractional
window always has one packet in flight, so every drop costs about 200 ms
(E6 packet capture, report 2). GSwifT now offers two fixes from inside the
module: it can lower the socket's minimum RTO at connection start
(`bpf_setsockopt(TCP_BPF_RTO_MIN)`, which also shortens the probe), and it
can keep two packets in flight below two packets per RTT, sent as one
two-segment burst per pacing gap, so that a single loss is repaired by SACK
(`--min-snd-cwnd 2`). Section 6.5 shows that incast needs both.

### 4.4 Linux's loss recovery acts before the controller

Linux treats reordering as loss: sprayed packets trigger fast recovery,
which cuts the window regardless of the delay signal. MSwift's filter only
matters once recovery tolerates reordering (RACK off, `tcp_reordering` 64),
which is a host-wide setting rather than something the module controls.

### 4.5 The verifier and the per-socket budget

Flow-based scaling needs `1/sqrt(cwnd)` on every ACK. Our branch-free,
fully unrolled integer square root exceeded the verifier's one-million
instruction budget, so earlier versions compiled flow scaling out. GSwifT
now has the loader compute the scaling term once for 592 window buckets
(steps of 1/256 packet below one packet, 1/32 up to 8, 1/2 up to 64, each at
its bucket's midpoint) and store the table in the configuration map; the BPF
program does one bounded array read. The table stays within 4 µs of the
exact formula (unit test). Per-socket state must fit in 104 bytes, which caps
MSwift's median window at 16 samples of 16 bits.

## 5 Methodology

### 5.1 Testbeds

Three sessions on CloudLab Utah, two nodes each, connected through the
cluster switch at 25 Gb/s:

| Session | Nodes | CPU / NIC | Kernel | 64 B RTT p50 / p99 |
|---|---|---|---|---|
| 1 (Nov 9) | 2 x c6525-25g | AMD EPYC 7302P / ConnectX-5 | Ubuntu 6.8.0-138 | 42.2 / 55.3 µs |
| 2 (Nov 10) | 2 x xl170 | Intel E5-2640 v4 / ConnectX-4 Lx | mainline 6.8.0 | 38.5 / 53.9 µs |
| 3 (Nov 20) | 2 x c6525-25g | AMD EPYC 7302P / ConnectX-5 | Ubuntu 6.8.0-149 | 38.3 / 51.3 µs |

All results in this paper come from session 3, which used the corrected
harness of Section 5.4. MTU 1500, interrupt coalescing off, quick ACKs on
the experiment route, `tcp_slow_start_after_idle` 0, CPU governor
`performance` on the sender.

### 5.2 Emulated bottleneck

With two nodes there is no shared switch bottleneck, so we build one on the
receiver. All traffic arriving at the receiver is redirected through an IFB
device, rate-limited to 10 Gb/s by HTB with a 1 MB token bucket, and queued
in a single FIFO: `fq_codel` with one flow queue, CoDel disabled,
`drop_batch 1`, bounded at the byte equivalent of 300 packets (100 for
incast). Packets queued longer than 50 µs are CE-marked; only DCTCP
negotiates ECN. A monitor samples the FIFO backlog every 10 ms. For E11 and
some RPC runs we also use a second design: the receiver's applications run
in a network namespace behind a veth pair, and the same HTB and FIFO shape
the veth egress, which avoids IFB's single-threaded receive path.

**What the emulator cannot do.** Two-ended captures show that the IFB and
HTB path itself adds delay that the FIFO backlog does not show: about 80 µs
when Swift holds the queue near empty at a 100 µs target, about 35 µs at
300 µs, against 4-6 µs of receiver turnaround without the bottleneck. With a
smaller token bucket (256-512 KB) HTB throttled often enough that this
hidden delay collapsed Swift under an RPC load it handled easily without
HTB. Targets below about 150 µs therefore measure the emulator rather than
Swift; Section 6.3 tests them without a bottleneck and with the veth design.
HTB occasionally passed 2-6% above its configured rate (two of about 40
baseline runs).

### 5.3 Workloads and metrics

- **Bulk:** 8 flows, one iperf3 process per flow, 15 s, with a 64-byte
  request/response probe whose responses cross the bottleneck (RPC latency).
- **Incast:** N connections each return 64 KB at the same moment, 100 rounds;
  round completion time is the slowest flow's.
- **RPC workloads:** an open-loop generator with Poisson arrivals over 128
  persistent connections (one outstanding RPC per connection); response
  sizes from the DCTCP web-search distribution truncated at 1 MB (mean
  386 KB, as Homa's W5) or a synthetic short-RPC mix (half under 1 KB, mean
  37 KB); load is the response bytes as a fraction of the bottleneck.
  Slowdown is latency divided by the unloaded 64 B RTT plus transmission
  time at 10 Gb/s.
- **Spraying:** a tc-BPF classifier at the receiver's bottleneck sends each
  data packet, independently with probability q, through a netem path that
  adds 300 or 1000 µs; GRO is off so each wire packet is a separate choice
  (1 Gb/s bottleneck).
- **Telemetry:** per-ACK module records, nstat counters, qdisc counters,
  and for E7 header captures on both hosts.

### 5.4 Four harness pitfalls

Our first two sessions and the start of the third used a harness with four
flaws. We found them while explaining Swift's collapse at low targets; the
evidence is in `results/2026-11-20`.

1. **A single-threaded traffic generator couples flows.** iperf3 3.9 drives
   all `-P` streams from one thread with blocking writes. When one Swift flow
   drops to a window of 0.01 packets (one packet every ~30 ms), its full send
   buffer blocks the writes of every other stream. At a 100 µs target no flow
   was sending in 77% of 1 ms intervals. With one process per flow, Swift's
   utilization at 100 µs doubled (7-12% to 18-23%, three interleaved
   repeats); Cubic and DCTCP were unaffected.
2. **An skb-counted buffer is not a packet-counted buffer.** The FIFO runs
   after GRO, and its 300-packet limit counted GRO aggregates of 5-55 KB, so
   the buffer held up to about 16 MB. Turning GRO off fixes the accounting
   but caps the IFB path at 2.9 Gb/s, so we keep GRO on and bound the FIFO
   in bytes (`memory_limit`, 2,180 bytes of skb memory per 1,514-byte
   packet, measured): Cubic now fills it to 319 µs with 0.8% retransmissions.
3. **A global ECN setting changes the loss-based baseline.** We enabled
   `tcp_ecn=1` for DCTCP. Applications choose the congestion control after
   `connect()`, so DCTCP's own ECN request never reaches the SYN and only the
   global setting enabled ECN, for every algorithm: our "Cubic" halved its
   window on the 50 µs marks meant for DCTCP and filled 80% of the link.
   The sender's `tcp_ecn` is now set per run, 1 only for DCTCP; Cubic fills
   96%.
4. **iperf3's receiver-side rate can be wrong.** In 6 of 45 pacing runs the
   receiver-side duration was inflated (18 s for an 8 s test) while the byte
   count was right, under-reporting goodput 2.2x. We compute goodput from
   received bytes over the sender's duration.

These flaws affect the Cubic and Swift numbers in our progress reports.

### 5.5 Repetition and statistics

Each sweep repeats its configurations 2-5 times with the repeat loop
outermost, so repeats of one configuration are spread over the session and
interleaved with the alternatives. We report medians with the min-max range
in parentheses.

## 6 Evaluation

### 6.1 Basic latency and throughput

| | Value |
|---|---|
| Unloaded 64 B TCP RTT (p1 / p50 / p99) | 37.0 / 38.3 / 51.3 µs |
| Swift, 8 flows, no bottleneck (sender NIC 25 Gb/s), any target 50-300 µs | 22.5-23.6 Gb/s |
| 8 flows through the 10 Gb/s bottleneck: Cubic / DCTCP / Swift (200 µs) | 95.7% / 96.9% / 95.5% |
| Bottleneck queue p99: Cubic / DCTCP / Swift (200 µs) | 391 / 160 / 174 µs |
| 64 B RPC p99 under that load: Cubic / DCTCP / Swift (200 µs) | 519 / 453 / 455 µs |
| Retransmitted segments: Cubic / DCTCP / Swift | 0.79% / 0.01% / 0% |

### 6.2 Delay-signal fidelity

Table 7.1 (Section 7) splits the module's RTT sample with captures on both
hosts. Two results matter for Swift. First, the part of the sample that
grows with load is below TCP, in the NIC, driver and interrupt paths, not in
the receiver's TCP stack (4-6 µs at every load). Second, it grows with the
amount of data in flight, not with the rate as such: with the window bounded
by a 50 µs target, eight Swift flows still fill the 25 Gb/s NIC (Section
6.3).

**Host settings do not remove it** (E10, no bottleneck, Swift with an
unreachable target, module RTT p50):

| Setting | one flow at 3 Gb/s | one flow, unlimited | eight flows, unlimited |
|---|---|---|---|
| default | 85 µs | 332 µs (16.1 Gb/s) | 2293 µs (23.4 Gb/s) |
| byte queue limit 64 KB | 89 | 329 | 3133 |
| TCP small queues 128 KB | 87 | 330 | 3107 |
| busy polling 50 µs | 90 | 323 | 2844 |
| receiver GRO off | 244 | 754 (6.4 Gb/s) | 2010 |
| sender TSO off | 174 | 175 (5.3 Gb/s) | 4414 (19.5 Gb/s) |

Limiting the NIC queue or TCP's queued bytes does not lower the delay at
moderate load, and removing batching (GRO, TSO) raises it: per-packet
processing costs more than the batching it replaces. The eight-flow column
is dominated by windows that never shrink and is noisy.

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

**Below 150 µs the limit is the emulator, not Swift** (E11, three repeats):

| Target | No bottleneck | IFB bottleneck | veth bottleneck |
|---|---|---|---|
| 50 µs | 22.5 Gb/s | 1.7 | 3.2 |
| 75 µs | 23.5 | 1.7 | 4.7 |
| 100 µs | 23.5 | 1.8 | 7.7 |
| 150 µs | 23.5 | 9.3 | 9.6 |
| 200-300 µs | 23.5 | 9.6 | 9.6 |

When the sender's own NIC is the bottleneck, eight Swift flows run at line
rate at every target down to 50 µs. Both software bottlenecks impose a floor,
the veth design a lower one; Section 5.2 measures the delay they add. A
delay-controlled sender on Linux can hold RTT near 50 µs at line rate, but a
timer-driven software shaper cannot emulate a switch at that scale.

**Near the floor, Swift is bistable, and per-flow target diversity removes
it.** At 150 µs one repeat in five collapsed to about 30% in each of three
independent blocks. With a per-flow random offset (each flow's target is
base + U[0, J), drawn at connection start) and the same mean target, every
repeat held 91-96% (base 130 with J = 40; base 110 with J = 80). At a mean of
100 µs the offset raised utilization from 17-28% to a median of 80%
(38-93%). The floor-relative target behaves the same way, since flows'
minimum RTTs differ (per-flow targets 132-158 µs at +100 µs). A control
rules out the simpler explanation that the flows with the highest targets
carry the load: with every flow at 120 µs, the highest target any flow drew
with base 80 and J = 40, utilization was 19-28%, against 66-91% with the
jittered targets (five interleaved repeats each). We attribute the effect to
de-synchronization: flows with identical targets cross the target together
and cut together.

### 6.4 RPC workloads

Short RPCs (under 10 KB) are where Swift is meant to help. Table 6.4 gives
their 99th-percentile slowdown (two or three repeats; ranges in
`tables/e8_summary.csv`); `figs/e8_slowdown_*` plot slowdown against message
size, as Homa's Figure 3 does.

| Bottleneck | Workload | Load | Cubic | DCTCP | Swift, 200 µs |
|---|---|---|---|---|---|
| IFB | web-search | 50% | 15.7 | 15.2 | 13.1 |
| IFB | short-RPC mix | 50% | 16.2 | 15.7 | collapses in 2 of 3 repeats |
| veth | web-search | 50% | 16.9 | 14.5 | 14.2 |
| veth | short-RPC mix | 50% | 17.9 | 18.3 | 16.1 |
| veth | web-search | 80% | 93 | 89 | 436,000 |
| veth | short-RPC mix | 80% | 120 | 173 | 515,000 |

**At 50% load Swift matches Cubic and DCTCP** on both workloads with the
veth bottleneck; on IFB one workload was bistable. At a 100 µs target Swift
failed every RPC run, as Section 6.3 predicts.

**At 80% load Swift collapses** on both bottlenecks, while DCTCP keeps short
RPCs within 100-200x of ideal and Cubic, on veth, does similarly. Swift then
delivers the offered rate only with a backlog of pending RPCs that grows
through the run. Its RTT samples at window cuts are 380-680 µs while the
bottleneck queue is nearly empty (p99 4 KB), most ACKs arrive with windows
near one packet, and it uses about twice DCTCP's CPU on both hosts (8-10
receiver cores against 3-4). We tried four remedies (two repeats each,
veth):

| Swift variant at 80% load | web-search p99 slowdown | short-RPC p99 slowdown |
|---|---|---|
| target 200 µs | 436,000 | 515,000 |
| target 300 µs | 324,000 | 456,000 |
| + flow scaling, range 400 µs | 400,000 | 470,000 |
| + Linux window floor 2 | 356,000 | 436,000 |
| + Linux window floor 4 | 35,000 | 124,000 |
| 32 connections instead of 128 | 553,000 | 572,000 |

Sending bursts of four packets per pacing gap helps by 4-10x, consistent
with per-packet sending cost being part of the problem, but none of these
closes the gap to DCTCP. We leave the mechanism open (Section 9).

### 6.5 Incast

N connections on one sender each return 64 KB to the receiver at the same
moment, into a 100-packet buffer; 100 rounds per run, three repeats.
Table 6.5 gives the 99th-percentile round completion time (median over
repeats; repeats differ by under 10% in almost every cell).

| Configuration | N = 32 | 64 | 128 | 256 |
|---|---|---|---|---|
| Cubic | 209 ms | 215 | 633 | 3115 |
| DCTCP | 206 | 218 | 631 | 3115 |
| Swift, fractional window | 209 | 213 | 370 | 887 |
| Swift, one-packet floor | 10 | 23 | 222 | 617 |
| Swift, all fixes (two in flight, `rto_min` 5 ms, min-RTT pacing) | 13 | 25 | 53 | 124 |
| Cubic, `rto_min` 5 ms | 8 | 21 | 51 | 121 |
| DCTCP, `rto_min` 5 ms | 8 | 21 | 46 | 102 |
| Swift, one-packet floor, `rto_min` 5 ms | 10 | 23 | 47 | 102 |

**Loss recovery, not the control law, decides incast completion time.**
With Linux's default 200 ms minimum RTO, every configuration that loses a
packet in most rounds finishes near a multiple of 200 ms: Cubic and DCTCP
at every fan-in, Swift with a one-packet floor from N = 128. With a 5 ms
minimum RTO all algorithms come within 20% of each other (102-124 ms at
N = 256).

**Fractional windows need both lone-packet fixes.** Swift's sub-packet
window as published is the slowest Swift variant because each dropped lone
packet waits for the tail-loss probe (Section 4.3). Keeping two packets in
flight without a lower minimum RTO helps at N = 32 (27 ms) but is worse than
the plain window at N = 256 (1507 ms); a lower minimum RTO alone cuts N = 64
to 49 ms. Together with min-RTT pacing they bring the fractional window to
124 ms at N = 256, 5x faster than the one-packet floor at the default RTO,
and level with Cubic and DCTCP at a 5 ms RTO.

**Swift's lasting advantage is loss avoidance.** At N = 64 Swift with a
one-packet floor retransmits 0.13% of segments, against 16.3% for Cubic and
19.4% for DCTCP; at N = 256, 2.1% against 21.7% and 26.6% (default RTO).
Fewer losses do not make rounds faster once recovery is cheap, but they
matter wherever retransmissions cost bandwidth or CPU.

**Distance from the ideal.** Draining N x 64 KB at 10 Gb/s takes 3.4 ms at
N = 64 and 13.4 ms at N = 256. The best configurations' median round times
are 13-23 ms and 60-110 ms, 4-8x the ideal.

### 6.6 Multipath spraying and MSwift

Each data packet takes, with probability q, a slow path adding 300 or
1000 µs; the measured share matched q within 0.02 in every run. Loss
recovery is reordering-tolerant (Section 4.4). Base target 100 µs, two
repeats, which agree within 0.02 Gb/s. Throughput through the 1 Gb/s
bottleneck:

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
| 8 | +1000 µs | Swift | 0.95 | 0.95 | 0.40 | 0.08 |
| 8 | +1000 µs | MSwift, k = 4 | 0.95 | 0.95 | 0.95 | 0.42 |

With q = 0 every variant delivers 0.95 Gb/s. At +100 µs no variant lost
throughput at any q. Cubic holds 0.95 Gb/s at every q, at the cost of
700-1,050 fast recoveries per run.

**Swift collapses faster than the model predicts.** Once the decline starts,
it is much steeper than `1/sqrt(q)`: for one flow at +300 µs throughput falls
5x from q = 0.1 to 0.35 (a log-log slope of about -1.3), for two flows 2x
from 0.25 to 0.35 (about -2). With 8 flows each window is about one packet
and the decline starts only at q = 0.35.

**The median helps only as far as its window reaches.** At 1 Gb/s one
flow's window is about nine packets, so the median spans four samples and
holds full throughput to q = 0.1 but not 0.25; with two flows it holds to
0.25. With eight flows the median degenerates to the latest sample and
MSwift behaves like Swift.

**Reordering tolerance carries most of the benefit.** Decreasing only after
four consecutive over-target samples holds full throughput to q = 0.25 (one
flow), 0.35 (two flows) and 0.5 (eight flows, +300 µs). It needs no window
of samples, so it works where the median cannot.

**With Linux's default loss recovery**, sprayed reordering triggers tens of
thousands of fast recoveries per run for every algorithm (8 flows, +300 µs);
at 1 Gb/s these cost all variants only 0.02-0.06 Gb/s up to q = 0.25.

### 6.7 Queue occupancy

Across the bulk runs, the bottleneck's p99 occupancy is set by the
algorithm's operating point rather than its throughput: Cubic fills the
300-packet buffer (391 µs), DCTCP holds it near its 50 µs marking threshold
(p50 0, p99 160 µs), and Swift holds it at its target minus the host's share
of the RTT (p99 148, 174 and 277 µs at targets of 150, 200 and 300 µs). In
incast all algorithms overflow the 100-packet buffer from N = 64; Swift's
smaller windows make that overflow rarer, which is the retransmission gap in
Section 6.5.

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
stack and qdisc add little until the window is unlimited. Almost all of the
sample, and all of its growth with load, sits below TCP: in the NIC queues,
drivers, interrupt handling and GRO of both hosts. This is the delay
Swift's NIC timestamps exclude and its endpoint/fabric split attributes. The
large values in the last row come from a window that never shrinks; a
window bounded by a target also bounds these queues (Section 6.3).

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

The control law is cheap: 131 ns per ACK, under 0.02 cores at the roughly
125,000 calls per second of these runs. Telemetry through the ring buffer
costs up to 18x that, so evaluation runs sample one ACK in 4 or 16. MSwift's
median is quadratic in its window and costs 6x Swift's update, still small.
Swift and DCTCP each use about 2.5x Cubic's sender CPU at the same
throughput; both keep windows and queues small, which plausibly means
smaller transmissions per send, though we did not measure segment sizes.
Under the RPC workloads of Section 6.4 the gap grows: at 80% load Swift used
2.5-4 sender cores and 8-10 receiver cores against DCTCP's 1.5 and 3-4.

## 8 Related work

**Delay-based datacenter congestion control with hardware support.**
TIMELY [Mittal et al., SIGCOMM 2015] reacts to the RTT gradient measured
with NIC timestamps for RDMA traffic. Swift [Kumar et al., SIGCOMM 2020]
switched to an absolute target delay, split fabric from endpoint delay, and
added sub-packet windows; Falcon [Singhvi et al., SIGCOMM 2025] moves a
two-window Swift into NIC hardware. All three take precise timestamps and
fine-grained pacing as given. We remove those assumptions and measure what
happens to the same control law when the delay signal is the kernel's
software RTT and pacing is Linux's internal pacer. Our finding that most of
the signal is host time, below TCP, is the reason those systems rely on
hardware.

**Multipath spraying.** MSwift [Gerstein et al., 2025] models Swift under
packet spraying and proposes the median filter; its evaluation is analytical
and simulated. We test the filter on a real stack with the spray choice made
per wire packet, and find that its benefit depends on window size and on
making Linux's own loss recovery tolerate reordering.

**Datacenter transports re-implemented in Linux.** Homa/Linux [Ousterhout,
ATC 2021] re-implemented a transport first evaluated in simulation and a
kernel-bypass system, to measure which benefits survive the kernel; it found
that software overheads, not network congestion, limit latency. Our study
has the same method and a related conclusion with a different consequence:
for a delay-based controller, software overheads do not only add latency,
they enter the control loop as false congestion. Unlike Homa, Swift needs no
new transport, so GSwifT keeps TCP and its API and replaces only the
congestion control.

**Congestion control in BPF.** Linux has accepted TCP congestion controls as
BPF `struct_ops` programs since kernel 5.6; the kernel's own self-tests port
DCTCP and Cubic this way. GSwifT uses the same interface for an algorithm
whose state and pacing needs go beyond those ports (sub-packet windows,
per-ACK median filtering, a per-ACK square root), and documents where the
interface constrains it (Sections 4.2 and 4.5).

**Baselines.** Cubic [Ha et al., 2008] is Linux's default loss-based
algorithm. DCTCP [Alizadeh et al., SIGCOMM 2010] reacts to the fraction of
ECN-marked packets and is the standard datacenter baseline that, like
Swift, targets low queues; unlike Swift it needs switch marking, which our
bottleneck provides.

## 9 Limitations and future work

- **No hardware bottleneck.** Every congested result uses a software shaper
  on the receiver, which adds 35-80 µs of its own delay; targets below
  150 µs, and possibly the 80% RPC collapse, need a real switch bottleneck
  (several senders through one switch port) to settle.
- **No endpoint/fabric split.** The receiver would have to report its own
  queuing time (for example in a TCP option) for GSwifT to separate host
  delay from network delay as Swift does; Section 7 shows that most of that
  delay is in the NIC and driver paths, which even the receiver's TCP stack
  does not see.
- **The 80% RPC collapse is unexplained.** RTT inflates outside the network
  queue and CPU doubles; larger bursts help. A packet-level and CPU-profile
  study at 80% load is the next step.
- **Two nodes.** Incast comes from many connections on one sender, not many
  hosts, so sender-side contention may differ from a real fan-in.
- **One kernel family.** Pacing accuracy changed between two builds of the
  same kernel version; results on other kernels need the pacing check of
  Section 4.2.

## 10 Conclusion

Swift's control law fits comfortably in a Linux BPF congestion control and,
given a target of about 200 µs, holds a 10 Gb/s link at DCTCP's utilization
with a bounded queue and no loss. What does not carry over is the delay
signal: on Linux most of each RTT sample is host time below TCP, which a
congestion control cannot separate from the network, and which sets a floor
under the target. Near that floor Swift is unstable, and giving flows
slightly different targets is a simple fix. Sub-packet windows work only
with Linux-specific help, and then match, rather than beat, loss-based
algorithms that are given a short minimum RTO, while losing far fewer
packets. MSwift's reordering tolerance carries over well; its median filter
needs windows of several packets. The open problem is Swift under heavy
open-loop RPC load, where Linux's per-packet costs appear to feed back into
the delay signal. Finally, emulating a datacenter bottleneck in software is
harder than it looks: four common shortcuts each changed our results by up
to an order of magnitude, and the shaper's own delay limits how low a
target can be tested.
