# GSwifT: Progress Report 1

**Delay-Based Congestion Control on Commodity Linux**

Ethan Kim, Gregory Park, Niki Hu, Xiaomeng Ye, Raymond Zhao
Georgia Tech, Fall 2026

## 1. Problem Definition

Swift (Kumar et al., SIGCOMM 2020) is Google's delay-based datacenter congestion
control. It holds end-to-end delay near a target by adjusting the congestion
window with AIMD (additive increase, multiplicative decrease). Three properties
are central to it:

1. **Precise delay measurement.** Swift measures RTT using NIC hardware
   timestamps, so the signal reflects queuing rather than host software noise.
2. **Separating fabric from endpoint delay.** Swift splits RTT into network
   (fabric) delay and endpoint (host) delay and keeps a separate window for each.
   That way a slow receiver is not mistaken for a congested network.
3. **Sub-packet windows for large incast.** Swift lets cwnd fall below 1 and
   paces packets more than one RTT apart, so hundreds of senders can share one
   receiver without overflowing its buffer.

All three rely on Google's infrastructure: NIC timestamps, an OS-bypass transport
(Pony Express on Snap), and fine-grained pacing. **We ask how much of Swift's
benefit carries over to stock Linux TCP**, implemented as an eBPF `struct_ops`
congestion control module on unmodified kernels and commodity NICs.

Concretely, we test three portability questions:

- **RQ1 (measurement):** Is software-timestamped RTT accurate enough to drive
  Swift's control loop? How large is the gap to hardware timestamps?
- **RQ2 (fabric vs endpoint):** Can a Linux sender tell network congestion apart
  from receiver congestion, and does acting on that split help?
- **RQ3 (incast):** Can pacing-based fractional windows handle large fan-in
  (tens to hundreds of flows) better than Cubic and DCTCP?

As an extension, we evaluate **MSwift** (Gerstein et al., 2025), which makes Swift
robust when packets are sprayed across paths with different delays.

## 2. Motivation

Delay is a simple, informative congestion signal. It doesn't need switch support for ECN marking or in-network telemetry.
At Google, Swift kept tail latency low at high utilization. A version that runs
on stock Linux would bring those benefits to any datacenter operator, not just
those with custom transport stacks.

Our own measurements show host noise is the main obstacle. On our testbed,
a 64-byte TCP round trip takes 71.8 µs at the median. Only 2.47 µs of that is
the network; the other 97% is host software on the two endpoints
(Section 4.2).

So a delay-based algorithm that sees software RTT is mostly measuring host
noise. That noise varies by tens of µs, while the network delay it is trying to
control is 2.5 µs. This makes Swift's fabric/endpoint split essential, not an
optimization. Adoption is easy as multi spraying is becoming common, as working module could be deployed by anyone running a modern Linux kernel.

## 3. Challenges

**Measurement fidelity.**
- Linux RTT samples are software timestamps. They include interrupt handling,
  softirq scheduling, and socket wakeup on both hosts.
- Linux batches ACKs, so samples are not per packet. On our testbed a 2 Gbps
  flow produced about one RTT sample per 7 packets (Section 4.4).
- Segmentation offloads (TSO when sending, GRO when receiving) merge and split
  packets. Per-packet timestamps then stop lining up with application messages
  (Section 4.2).

**Hardware timestamps are not available to congestion control.**
- The NIC timestamps every packet, but the timestamps reach only the socket
  layer (`SO_TIMESTAMPING`), not the kernel's congestion-control hooks.

**Linux's window cannot go below 1.**
- `snd_cwnd` is an integer with a floor of 1. We are unable to produce this part of optimization that the paper mentioned.

**BPF constraints.**
- The verifier limits loops, memory access, and per-socket state. The module gets
  roughly 100 bytes of private per-socket state.
- MSwift's median over cwnd/2 samples does not fit in that space without a cap
  or a separate map.

**Creating real congestion at 100 Gbps.**
- One kernel TCP flow on our nodes reaches only about 21 Gbps, so two nodes and
  one flow never fill the link. We plan to manually create congestion.

## 4. Methodology

### 4.1 Hardware platform

Tested: CloudLab, Clemson cluster, node type r6525, bare metal.

| Component | Value |
|---|---|
| CPU / RAM | AMD EPYC Milan, 64 cores (128 threads), 256 GB |
| OS / kernel | Ubuntu 24.04, 6.8.0-138-generic (stock image, no custom kernel) |
| Experiment NIC | Mellanox ConnectX-6 Dx (MT2892), `enp129s0f0np0`, PHC 2 |
| Control NIC | Mellanox ConnectX-5 (MT27800), `eno12399np0` |
| Experiment link | 100 GbE, direct attach copper to the cluster switch, MTU 9000 |

**Why r6525 qualifies.** Swift needs the NIC to timestamp *every* received
packet. Many NICs only support the PTP-only filter modes, which timestamp just
PTP packets. Linux reports this capability as the receive filter mode `all`
(`HWTSTAMP_FILTER_ALL`). Both Mellanox cards on r6525 report it.

**Other node types.** Any CloudLab node type can be qualified with the same
criterion. Our script `verification/scripts/verify.sh` captures the NIC model,
timestamping capabilities, BPF kernel configuration, and tuning state of any
node, and flags a node whose experiment NIC lacks `HWTSTAMP_FILTER_ALL`. We
standardize on r6525 so results are comparable across sessions.
Possible other nodes include: xl170, c6525-25g, c6525-100g, d6515, r6615, r6525, r650, r7525, nvidiagh, sm110p, sm220u, d7525, d8545

### 4.2 Verifying hardware timestamping

We validated hardware timestamping at three increasing levels of rigor.

**Level 1: capability.** `ethtool -T` on both nodes reports `hardware-transmit`,
`hardware-receive`, `hardware-raw-clock`, and receive filter mode `all`.
`hwstamp_ctl -i enp129s0f0np0 -t 1 -r 1` sets the NIC to timestamp all
received packets; the driver accepted it (`rx_filter 1`) on both nodes.

**Level 2: clock synchronization (PTP).** We ran `ptp4l -H` with node0 as
master and node1 as slave:
- The slave reached the locked servo state (`s2`) within 2 s, starting from a
  362 ms initial offset.
- Over the next 212 s, the master offset stayed within ±18 ns, and the measured
  path delay stayed between 1337 and 1344 ns.

This shows the hardware clocks and timestamp pipeline work.

**Level 3: hardware timestamps on TCP data (the property Swift needs).** We wrote
a TCP ping-pong tool, `verification/scripts/hwts_pingpong.c`. It enables
`SO_TIMESTAMPING` hardware TX and RX timestamps on both ends:

```
client NIC tx ───────────────► server NIC rx
                                   │ server residence (host stack + app)
client NIC rx ◄─────────────── server NIC tx

network RTT = (client NIC rx − client NIC tx) − (server NIC tx − server NIC rx)
```

Each difference uses two timestamps from one NIC's clock, so the result does not
depend on synchronizing the two machines. It is the same decomposition Swift
uses. We ran 20,000 round trips at each message size.

| 64 B messages (µs) | p1 | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Application RTT | 63.0 | 71.8 | 98.5 | 107.6 |
| Server residence (NIC rx to NIC tx) | 39.0 | 41.6 | 56.5 | 66.4 |
| Client host (application RTT minus NIC RTT) | 20.7 | 25.3 | 50.3 | 58.6 |
| **Network (wire + switch, both directions)** | **2.459** | **2.471** | **2.493** | **2.499** |

**How we judge that timestamping works correctly:**

1. **Completeness.** 0 of 60,000 round trips (64 B, 4 KB, 64 KB) had a missing
   hardware timestamp on either end.
2. **Precision.** The network component varies by only 34 ns from p1 to p99
   (2.459 to 2.493 µs). The host components vary by tens of µs. Software
   timestamps, or a timestamp taken at the wrong layer, would show
   microsecond-scale jitter in the network column.
3. **Consistency with an independent method.**
   - PTP measured 1.34 µs one way, which implies 2.68 µs round trip; the
     ping-pong measured 2.47 µs. The two agree to within 0.2 µs.
   - 4 KB messages raise the network RTT to 2.99 µs, consistent with longer
     time to put larger frames on the wire.

**Additional baseline.** Tuned ICMP RTT is 64 µs min / 134 µs avg at one ping
per second, but 23 µs / 26 µs at one ping per millisecond. Sparse probes mostly
measure idle-host wakeup. We therefore report baselines at a stated probe rate
and treat TCP request/response RTT (neper `tcp_rr`) as the reference.

### 4.3 Swift as a BPF congestion control module

The module implements Swift's Algorithm 1 through the kernel's `cong_control`
hook. That hook runs on every ACK and receives a `rate_sample` that includes an
RTT sample. Our starting reference is the kernel's `bpf_dctcp` selftest.

We plan to implement the following components:

1. **Target-delay AIMD.**
   - Delay below target: increase cwnd by `ai / cwnd` per ACK (by `ai` per
     ACK when cwnd < 1).
   - Delay above target: decrease cwnd multiplicatively by
     `β × (delay − target) / delay`, capped at a maximum decrease factor, at
     most once per RTT.
   - Retransmission timeouts and fast recovery handled as in the Swift paper.
2. **Fractional window and pacing.**
   - A fixed-point fractional cwnd is kept in module state.
   - Linux `snd_cwnd` is set to `max(1, floor(cwnd))`.
   - When cwnd < 1, `sk_pacing_rate` is set so packets leave `RTT / cwnd` apart.
3. **Flow-based target scaling.** The target grows as `α / sqrt(cwnd) + β`,
   clamped to a range. This favors fairness among many small flows.
4. **Topology-based scaling.** The target adds a per-hop term. TTL echoing is
   awkward inside TCP, so hop count is configured per destination through a BPF
   map, a simplification we will state.
5. **Fabric vs endpoint split (RQ2).** The receiver measures how long each packet
   waits inside its host and reports that back. The sender subtracts it from RTT
   to get fabric delay, and keeps two windows (fabric and endpoint); the
   effective window is the minimum of the two.
   - The receiver-side component is a BPF `sock_ops` program. It uses Linux's
     support for BPF-written TCP header options to carry the residence time in a
     custom TCP option.
   - The receiver measures residence time from its software receive timestamp
     (or its hardware timestamp, where we can obtain it) to the time the ACK is
     sent.

**Telemetry.** The module exports per-ACK records (time, RTT sample, target,
cwnd, pacing rate, decrease events) through a BPF ring buffer to a userspace
logger. These traces drive all delay and cwnd plots.

**Hardware timestamps in the loop (RQ1).** Hardware timestamps are visible to
sockets but not to `cong_control`. We use two approaches:
- **Primary:** run the software-timestamp module, and measure ground truth
  alongside it with the `SO_TIMESTAMPING` harness (Section 4.2). This
  quantifies how far the controller's input is from true fabric delay.
- **Stretch:** forward NIC timestamps into a BPF map that the module reads,
  then compare control quality between the two inputs.

### 4.4 Prototype: confirming the mechanisms work on the target kernel

We built and loaded a minimal module (`verification/scripts/cc_probe.bpf.c`) on
the stock 6.8 kernel, using `bpftool struct_ops register`. It holds cwnd fixed,
sets `sk_pacing_rate` on every ACK, and counts RTT samples.

We have verified that:
- Verifier accepts `cong_control` and writes to `snd_cwnd`, `sk_pacing_rate`, `sk_pacing_status`.
- 39,913 `cong_control` calls in a 10 s flow.
- Per-ACK RTT samples are available and recordable on every call.

We are confident that every kernel-side mechanism Swift needs is reachable from BPF on our platform without kernel changes.

**Observation.** About 4,000 RTT samples per second at 2 Gbps is roughly one per
7 packets, because of ACK batching. We will quantify how sample sparsity affects
control, and test receiver-side quick ACKs as a mitigation.

### 4.5 Prototype: sending less than one packet per RTT

Since Linux system does not support `cwnd < 1`, we are attempting to simulate this optimization strategy by implementing fractiona windows on Linux.
We rebuilt the probe with cwnd = 1 and pacing at 100 Mbps. At our RTT this is
about 0.06 packets per RTT, the regime Swift uses for large incast.

| Receiver configuration | Throughput |
|---|---|
| Default (delayed ACKs) | 2.3 Mbps |
| Route option `quickack 1` on receiver | **100 Mbps** (exactly the configured rate) |

With one packet in flight, the receiver holds the ACK until its delayed-ACK timer
fires. That limits the sender to about one packet every 31 ms, whatever the
pacing rate. With quick ACKs, pacing controls the rate precisely.

### 4.6 MSwift extension

MSwift is an extension to the Swift evaluation, not a completed component of the current prototype. It filters recent delay samples with a median and tolerates reordering so one delayed packet does not trigger an unnecessary window decrease. We plan to implement a median window with H = max(cwnd/2, 1), initially capped at 16 samples to fit BPF storage, and compare it with baseline Swift. Reordering tolerance will require k consecutive above-target samples before a decrease; planned k values are 1, 2, 4, and 8.

### 4.7 Packet spraying

**Spraying Setup**
A two-node link has only one path, so the initial experiment requires four nodes: sender, receiver, router, and background sender. The sender will use nftables per-packet random marking and policy routing to direct a selected fraction q of packets over a second path through the router. That path will have a tc-configured bottleneck and competing Cubic traffic; the remaining packets use a direct, uncongested path. ACKs will return on the direct path to isolate forward-path delay variation. We will sweep q = 0, 0.05, 0.1, 0.25, 0.5. If this controlled method works, a tc-eBPF IPv6-flow-label method with multipath routing is a more realistic follow-up.

**Verification and Comparison**
Spraying is considered valid only when three independent measurements agree:

Per-path packet and byte counters match the configured q within three binomial standard deviations.
Receiver packet captures and TCP reordering counters show reordering consistent with the delay difference between paths.
Delay samples show fast and delayed modes, with the delayed-mode fraction approximately q.
After this validation, we will compare baseline Swift and MSwift at each q, measuring throughput, delay, reordering, and loss. The hypothesis is that baseline Swift throughput degrades as more packets encounter the congested path, while median filtering makes MSwift's throughput-vs-q curve flatter. This remains planned work; no packet-spraying result is claimed yet.

Here's a diragram for spraying topology on four r6525 nodes:

```
                   path A (direct, uncongested)
  sender ─────────────────────────────────────────► receiver
     │                                                 ▲
     └──► router node ──(tc bottleneck + delay)────────┘
           path B (congested)      ▲
                                   │ competing Cubic traffic
                              background node
```

## 5. Related Work

**Swift: Delay is Simple and Effective for Congestion Control in the Datacenter**
(Kumar et al., SIGCOMM 2020).
- *What it is:* the core design we reproduce: target-delay AIMD with at most one
  decrease per RTT, a fabric/endpoint delay split with separate windows, cwnd
  below 1 with pacing, and a target scaled by hop count and by 1/√cwnd. It was
  deployed in Google datacenters on Pony Express/Snap with NIC timestamps.
- *How we differ:* we implement Swift inside standard Linux TCP as a BPF module,
  without OS bypass and without NIC timestamps in the control loop. Our
  contribution is measuring which of Swift's properties survive that
  translation, and why the others do not.

**Congestion Control for Spraying with Congested Paths (MSwift)**
(Gerstein, Silberstein, Keslassy, arXiv 2025).
- *What it is:* MSwift shows that when packets are sprayed and a fraction q hit a
  congested path, Swift's throughput falls roughly as 1/√q. It fixes this by
  reacting to the median of recent delay samples and tolerating reordering.
- *How we differ:* we check MSwift's model and fix on a real Linux stack and real
  hardware rather than in analysis and simulation. That includes interactions
  MSwift abstracts away: Linux's own reordering detection, ACK batching, and
  BPF limits on the median window.

**Falcon: A Reliable, Low Latency Hardware Transport**
(Singhvi et al., SIGCOMM 2025).
- *What it is:* Google's hardware transport. It builds a Swift variant into the
  NIC with two windows (fabric and NIC), hardware timestamps, and hardware pacing.
- *How we relate:* Falcon is the opposite end of the design space: full hardware
  support. We use it as the reference for what Swift's split looks like when
  hardware is available. Our endpoint/fabric decomposition measurement
  (Section 4.2) quantifies the gap a software implementation must work around.

**Baselines we compare against:**
- Cubic: the Linux default, loss-based.
- DCTCP: ECN-based, the standard datacenter comparison point.
- TCP Vegas: the classic delay-based algorithm in Linux.

## 6. Evaluation Plan

### 6.1 Questions and success criteria

| Question | Experiment | Success criterion |
|---|---|---|
| **RQ1** Is software RTT good enough? | Run the module while the `SO_TIMESTAMPING` harness records ground-truth fabric delay at a controlled bottleneck. Sweep load and target delay. | Report the error between software-RTT and NIC-measured fabric delay. **Success:** Swift-on-Linux keeps p99 fabric queuing within 2x the target while sustaining at least 90% of bottleneck throughput. **Negative result:** show where software noise makes this impossible (expected at small targets). |
| **RQ2** Does the fabric/endpoint split work? | Endpoint congestion scenario (Section 6.3, C). Run once with network-only congestion and once with endpoint-only congestion. | **Success:** with endpoint-only congestion, the endpoint window reacts and the fabric window does not (and vice versa). Throughput for competing flows not using the slow receiver is unaffected. Compare against a Swift variant without the split. |
| **RQ3** Does fractional cwnd handle incast? | Incast scenario (Section 6.3, A). Sweep fan-in from 10 to 500 flows. | **Success:** compared with Cubic and DCTCP at the same fan-in, lower drop and retransmission rate and lower p99 flow completion time. Module traces show cwnd < 1 engaged at high fan-in. |
| **Reproduction** Do Swift's paper trends hold? | Throughput and latency versus load; fairness among competing Swift flows. | Qualitative match: delay held near target as load rises; Jain's fairness index ≥ 0.9 among Swift flows. |
| **MSwift** | Spraying scenario (Section 6.3, D). Sweep q. | Baseline Swift throughput degrades with q roughly as 1/√q; MSwift's curve is measurably flatter at the same q. |

### 6.2 Traffic generators

- **neper** (Google's network performance tool).
  - `tcp_rr`: request/response RPC latency, the closest match to Swift's
    short-RPC tail-latency metric.
  - `tcp_stream`: bulk throughput with many flows per process. Supports setting
    the congestion control per socket.
- **iperf3** for simple long-lived bulk flows and as competing background traffic.
- **Custom incast generator** (to be written in C).
  - Off-the-shelf tools do not synchronize fan-in.
  - A coordinator on the receiver signals N sender processes, spread across
    sender nodes, to each send a B-byte response at the same instant.
  - It records per-flow completion time and per-round completion time. This is
    the classic partition/aggregate incast pattern.
- **hwts_pingpong** (our tool): ground-truth fabric and endpoint delay
  measurement running alongside the workloads.

### 6.3 Congestion scenarios

A single kernel TCP flow reaches about 21 Gbps on our nodes, and the
experiment link is 100 Gbps. Congestion must therefore be built on purpose. We
plan to use four scenarios, each targeting a specific question.

**A. Switch incast (RQ3).**
- Up to 16 r6525 senders, with many flows each, send to one receiver through
  the cluster switch.
- The switch port toward the receiver is the bottleneck, with a real switch
  buffer.
- Five or more senders at line rate already oversubscribe 100 Gbps. 500 flows
  are reached with about 32 flows per sender.

**B. Controlled router bottleneck (RQ1, reproduction).**
- Senders reach the receiver through a router node running a `tc` bottleneck:
  an HTB rate limit with a configurable FIFO size.
- This gives exact control over bottleneck rate and buffer.
- We can read queue occupancy (`tc -s qdisc`) as ground truth for queuing delay.

**C. Endpoint congestion (RQ2).**
- The receiver host is made the bottleneck while the network stays idle.
- Method: confine receive processing to a few cores (RSS/IRQ affinity) and run a
  CPU-bound application that reads slowly.
- Our 8-flow iperf3 run already reached line rate with 798 retransmits, so the
  receiver can be overloaded with modest effort.

**D. Congested path for spraying (MSwift).** The four-node topology in
Section 4.7, with path B's queue built by competing Cubic traffic at the router
bottleneck.

### 6.4 Metrics and how they are collected

| Metric | Source |
|---|---|
| RTT, target, cwnd, pacing rate, decrease events over time | Module ring buffer telemetry |
| Ground-truth fabric and endpoint delay | `SO_TIMESTAMPING` harness (NIC timestamps) |
| Throughput, per-flow and aggregate | neper / iperf3 |
| Flow completion time (mean, p99, p99.9) | neper `tcp_rr`, incast generator |
| Loss, retransmission, reordering | `nstat` TCP counters, receiver packet capture |
| Queue occupancy | `tc -s qdisc` on router, sampled |
| Fairness | Jain's index over per-flow throughput |

## 7. Progress and Next Steps

**Done:**
- Platform selected; per-session setup and reservation workflow documented.
- Hardware timestamping validated at all three levels, including on TCP data.
- Host versus network delay decomposition measured.
- Session tuning and verification scripts ready.
- BPF prototype confirms the per-ACK hook, RTT samples, enforced pacing, and
  sub-packet-per-RTT sending on the stock kernel.

**Next milestones:**
- Implementation of plans listed under Methodology section.
- Implementation and evaluation of MSwift
- (Stretch goal:) evaluation on real hardware, like MCX4121A-ACAT Network Card.
