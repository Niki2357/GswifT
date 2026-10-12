# GSwifT: Progress Report 1

**Delay-Based Congestion Control on Commodity Linux**

Ethan Kim, Gregory Park, Niki Hu, Xiaomeng Ye, Raymond Zhao
Georgia Tech, Fall 2026

---

## Response to proposal feedback

Each point from the proposal feedback, and where this report answers it.

| Feedback | Where addressed | Short answer |
|---|---|---|
| Provide a detailed evaluation plan | [Section 6](#6-evaluation-plan) | Baselines, workloads, sweep ranges, metrics, and a pass criterion for each research question. |
| Identify the hardware platform | [Section 4.1](#41-hardware-platform) | CloudLab r6525 (Clemson), ConnectX-6 Dx 100 GbE, Ubuntu 24.04, kernel 6.8. |
| Verify it supports hardware timestamping | [Section 4.2](#42-verifying-hardware-timestamping) | NIC supports `HWTSTAMP_FILTER_ALL`. PTP between two nodes locks within ±18 ns. |
| Which CloudLab machine types provide this | [Section 4.1](#41-hardware-platform) | r6525. The selection criterion and a per-node check script are given, so any other node type can be qualified the same way. |
| How you will validate the feature works correctly | [Section 4.2](#42-verifying-hardware-timestamping) | Done: TCP ping-pong with NIC timestamps on both ends. 0 missing timestamps in 60,000 round trips, network RTT 2.471 µs with 34 ns p1 to p99 spread. |
| How you will generate congestion scenarios | [Section 6.3](#63-congestion-scenarios) | Four scenarios: switch incast, controlled router bottleneck, receiver (endpoint) congestion, and a congested path for spraying. |
| Traffic generators | [Section 6.2](#62-traffic-generators) | neper, iperf3, and a custom synchronized incast generator. |
| Range of network conditions | [Section 6.4](#64-parameter-ranges) | Sweep table: fan-in, load, bottleneck rate, base RTT, buffer, target delay, competing traffic, spraying fraction. |
| MSwift implementation plan | [Section 4.6](#46-mswift-extension) | Median filter over the last H delay samples plus reordering tolerance, inside the same BPF module. |
| How packet spraying will be implemented | [Section 4.7](#47-packet-spraying) | nftables per-packet random marking plus policy routing over two paths, one of them deliberately congested. Flow-label spraying as a second step. |
| How you will verify spraying works | [Section 4.8](#48-verifying-spraying) | Three independent checks: per-path counters, receiver reordering, and a bimodal delay histogram. Numeric pass criteria given. |
| Confidence that infrastructure exists and mechanisms can be built | [Sections 4.2 to 4.4](#42-verifying-hardware-timestamping) | A working BPF congestion control prototype on the target kernel: per-ACK hook, RTT samples, enforced pacing, and sub-packet-per-RTT sending all demonstrated. |

---

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

**Delay is a simple, informative congestion signal.** It needs no switch support:
no ECN marking (as DCTCP needs) and no in-network telemetry (as HPCC needs).
At Google, Swift kept tail latency low at high utilization. A version that runs
on stock Linux would bring those benefits to any datacenter operator, not just
those with custom transport stacks.

**Our own measurements show host noise is the main obstacle.** On our testbed,
a 64-byte TCP round trip takes 71.8 µs at the median. Only **2.47 µs** of that is
the network; the other **97% is host software** on the two endpoints
(Section 4.2).

So a delay-based algorithm that sees software RTT is mostly measuring host
noise. That noise varies by tens of µs, while the network delay it is trying to
control is 2.5 µs. This makes Swift's fabric/endpoint split essential, not an
optimization. It also makes our question concrete: can that split be recovered
without Google's hardware path?

**eBPF makes deployment realistic.** `struct_ops` congestion control loads into
a stock kernel at runtime: no kernel rebuild, no reboot, no custom NIC driver.
A working module could be adopted by anyone running a modern Linux kernel.

**Multipath spraying is becoming common.** Load balancers such as Google's PLB
and packet-spraying fabrics deliberately send one flow's packets down different
paths. MSwift shows that plain Swift's throughput collapses when a fraction q of
sprayed packets hit a congested path. Whether that collapse, and MSwift's fix,
hold up under real Linux reordering behavior is untested.

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
- Bringing them into the control loop, or quantifying the cost of not doing so,
  is part of the work.

**The fabric/endpoint split requires the receiver's cooperation.**
- The sender cannot see how long a packet waited inside the receiver host.
- The receiver must measure that time and report it back. That means changing
  both ends of the connection without breaking standard TCP.

**Linux's window cannot go below 1.**
- `snd_cwnd` is an integer with a floor of 1. A fractional window has to live in
  the module's own state and be enforced through pacing.
- We found that pacing below one packet per RTT is defeated by the receiver's
  delayed ACKs unless quick ACKs are enabled (Section 4.5).

**BPF constraints.**
- The verifier limits loops, memory access, and per-socket state. The module gets
  roughly 100 bytes of private per-socket state.
- MSwift's median over cwnd/2 samples does not fit in that space without a cap
  or a separate map.

**Creating real congestion at 100 Gbps.**
- One kernel TCP flow on our nodes reaches only about 21 Gbps, so two nodes and
  one flow never fill the link.
- Congestion must be built deliberately, through fan-in, shaping, or a dedicated
  bottleneck node. The setup must also avoid host-side artifacts that look like
  network effects: with 8 iperf3 flows, half ran at about 16.5 Gbps and half at
  about 8.3 Gbps, likely because receive-side processing landed on shared CPU
  cores.

**Spraying collides with TCP loss recovery.**
- Linux TCP treats reordering as a possible loss signal.
- Spraying must be configured so that throughput changes come from the
  congestion controller under test, not from spurious retransmissions.

## 4. Methodology

### 4.1 Hardware platform

**CloudLab, Clemson cluster, node type r6525**, `small-lan` profile, bare metal.

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
standardize on r6525 so results are comparable across sessions. Clemson has 32
of them, enough for incast runs.

**Per-session procedure.** CloudLab tuning does not persist across experiments,
so each session runs, on every node:
- `tune.sh`: performance CPU governor, deep C-states disabled, interrupt
  coalescing off, comparison congestion control modules loaded.
- `verify.sh`: records the environment.

Every results folder records node type, kernel, interface, tuning state, and the
unloaded RTT baseline for that session.

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

This shows the hardware clocks and timestamp pipeline work. However, it only
exercises PTP packets.

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
4. **Known limit.** At 64 KB the decomposition breaks (p1 is negative). The tool
   pairs the request's last segment with the reply's first segment, and these no
   longer correspond once TSO and GRO split and merge packets. The Swift module
   must therefore work with per-packet (or per-ACK) samples, never per-message
   ones.

**Additional baseline.** Tuned ICMP RTT is 64 µs min / 134 µs avg at one ping
per second, but 23 µs / 26 µs at one ping per millisecond. Sparse probes mostly
measure idle-host wakeup. We therefore report baselines at a stated probe rate
and treat TCP request/response RTT (neper `tcp_rr`) as the reference.

### 4.3 Swift as a BPF congestion control module

The module implements Swift's Algorithm 1 through the kernel's `cong_control`
hook. That hook runs on every ACK and receives a `rate_sample` that includes an
RTT sample. Our starting reference is the kernel's `bpf_dctcp` selftest.

Components, in implementation order:

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

| Check | Result |
|---|---|
| Verifier accepts `cong_control` and writes to `snd_cwnd`, `sk_pacing_rate`, `sk_pacing_status` | Yes, loaded on first attempt |
| Hook runs on the data path | 39,913 `cong_control` calls in a 10 s flow |
| Per-ACK RTT samples available | Yes, on every call: min 34 µs, mean 44 µs |
| Pacing enforced (cwnd 200, pace 2 Gbps) | **2.00 Gbps** in every interval, 0 retransmits |

This retires the main implementation risk: every kernel-side mechanism Swift
needs is reachable from BPF on our platform without kernel changes.

**Observation.** About 4,000 RTT samples per second at 2 Gbps is roughly one per
7 packets, because of ACK batching. We will quantify how sample sparsity affects
control, and test receiver-side quick ACKs as a mitigation.

### 4.5 Prototype: sending less than one packet per RTT

We rebuilt the probe with cwnd = 1 and pacing at 100 Mbps. At our RTT this is
about 0.06 packets per RTT, the regime Swift uses for large incast.

| Receiver configuration | Throughput |
|---|---|
| Default (delayed ACKs) | 2.3 Mbps |
| Route option `quickack 1` on receiver | **100 Mbps** (exactly the configured rate) |

With one packet in flight, the receiver holds the ACK until its delayed-ACK timer
fires. That limits the sender to about one packet every 31 ms, whatever the
pacing rate. With quick ACKs, pacing controls the rate precisely.

**Conclusion:** fractional windows work on Linux, provided the receiver disables
delayed ACKs. This becomes part of our standard receiver configuration (and of
`tune.sh`), and we will state it as a deployment requirement.

### 4.6 MSwift extension

MSwift changes two things in Swift, and we add both to the same module behind a
runtime flag.

**1. Median-filtered delay.**
- Instead of reacting to each RTT sample, the controller reacts to the median of
  the last H samples, with `H = max(cwnd / 2, 1)`.
- BPF per-socket private state is too small for a large window, so we take two
  approaches and compare them:
  - **(a) Fixed window.** Cap H at 16. Keep samples in a small sorted array in
    per-socket storage (`BPF_MAP_TYPE_SK_STORAGE`). Insert in order and evict
    the oldest, with bounded loops the verifier can check. Median lookup is
    constant time.
  - **(b) Full window.** A ring buffer in per-socket storage sized to the largest
    H we test, with the median computed by a bounded selection. Used to check
    whether capping H at 16 changes results.

**2. Reordering tolerance.** Decrease only after k consecutive above-target
samples (k swept from 1 to 8), rather than on the first. On the Linux side,
`net.ipv4.tcp_reordering` and RACK settings are configured so that reordering
caused by spraying is not treated as loss. We report nstat retransmission
counters to show this holds.

### 4.7 Packet spraying

Spraying needs at least two paths, one of them congested. A two-node
`small-lan` has a single path, so the spraying topology uses four r6525 nodes:

```
                   path A (direct, uncongested)
  sender ─────────────────────────────────────────► receiver
     │                                                 ▲
     └──► router node ──(tc bottleneck + delay)────────┘
           path B (congested)      ▲
                                   │ competing Cubic traffic
                              background node
```

**Implementation step 1: nftables random spraying** (directly instantiates the
MSwift model, in which a fraction q of packets takes the congested path):

```bash
# sender: mark a fraction q of the flow's packets (example q = 0.25)
nft add table inet spray
nft add chain inet spray out '{ type route hook output priority mangle; }'
nft add rule inet spray out ip daddr $RCV tcp dport $PORT \
    numgen random mod 100 lt 25 meta mark set 0x1 counter
# marked packets use the routing table for path B, the rest use path A
ip rule add fwmark 0x1 table 100
ip route add $RCV via $ROUTER table 100
```

- Path B's congestion comes from a `tc` bottleneck on the router: a rate limit
  (for example 10 Gbps) and a configurable buffer.
- That bottleneck is shared with long-lived Cubic flows from the background
  node. This produces real queuing, rather than a fixed artificial delay.
- `netem` can add extra fixed delay on path B where we want a larger delay gap
  between the two paths.

**Implementation step 2: flow-label spraying** (closer to production practice):
- A tc-BPF egress program rewrites the IPv6 flow label per packet.
- The sender and router use multipath routing that hashes on the flow label
  (`fib_multipath_hash_policy` set to include the flow label).
- This mirrors how PLB-style load balancers and packet sprayers choose paths.

**Reverse path.** ACKs return over path A only, so the delay gap comes from the
forward direction alone. This keeps the setup aligned with the MSwift model.

### 4.8 Verifying spraying

We use three independent checks. Spraying is considered correct only if all
three agree.

1. **Counts.**
   - Measured from the nftables rule counter, `ethtool -S` and `ip -s link` on
     the router, and `tc -s qdisc` on the bottleneck.
   - Pass: the fraction of packets on path B is within 3 binomial standard
     deviations of q. For q = 0.25 over 10⁶ packets that is ±0.13 percentage
     points.
   - Byte counters cross-check that packet sizes are not skewed between paths.
2. **Reordering.**
   - Measured from receiver-side nstat counters (`TcpExtTCPSACKReorder`,
     `TcpExtTCPDSACKRecv`, `TcpExtTCPOFOQueue`), plus a packet capture on the
     receiver giving out-of-order fraction and reorder distance.
   - Expected: reordering is about zero at q = 0 and grows with q and with the
     path delay gap. A sprayer that is not spraying shows no reordering.
3. **Delay bimodality (the most direct check).**
   - Measured from a histogram of per-ACK RTT samples from the module's ring
     buffer, and independently of NIC-timestamped delays from the
     `hwts_pingpong` method.
   - Expected: two modes, separated by the path B queuing delay. The mass in the
     upper mode is about q.
   - This validates the exact assumption MSwift's analysis rests on.

**Target result:** sweep q and check whether baseline Swift's throughput falls
roughly as 1/√q as MSwift predicts, then show that the median filter flattens
that curve.

### 4.9 Development workflow

- All code and scripts live in the repository; CloudLab nodes are wiped at
  expiry.
- Logic is developed and unit-tested with network namespaces and veth pairs in
  a local Linux VM.
- CloudLab is used for timing-sensitive and congestion experiments.
- Results are copied into `results/<date>/` with a per-session `SUMMARY.md`
  before the reservation ends.

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
use four scenarios, each targeting a specific question.

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

### 6.4 Parameter ranges

| Parameter | Range | How set |
|---|---|---|
| Fan-in degree | 10, 50, 100, 200, 500 flows | Incast generator, flows per sender × sender nodes |
| Offered load | 20%, 40%, 60%, 80%, 100% of bottleneck | neper rate limits / incast inter-round gap |
| Bottleneck rate | 10, 25, 100 Gbps | Router `tc` rate (B, D); native 100G (A) |
| Base RTT | about 25 µs (one hop), plus 10, 50, 100 µs added | `netem` delay on router |
| Bottleneck buffer | 50 KB to 10 MB (shallow to deep) | Router qdisc limit |
| Swift target delay | 15 to 200 µs | Module parameter (BPF map) |
| Competing traffic | Swift alone; Swift vs Cubic; Swift vs DCTCP | Per-flow congestion control choice |
| Message size (RPC) | 64 B to 1 MB | neper / incast generator |
| Spraying fraction q | 0, 0.05, 0.1, 0.25, 0.5 | nftables `numgen` threshold |
| MSwift H, k | H ∈ {1, 4, 8, 16, cwnd/2}; k ∈ {1, 2, 4, 8} | Module parameters |

Each configuration runs at least 5 times. We report the median and spread.

### 6.5 Metrics and how they are collected

| Metric | Source |
|---|---|
| RTT, target, cwnd, pacing rate, decrease events over time | Module ring buffer telemetry |
| Ground-truth fabric and endpoint delay | `SO_TIMESTAMPING` harness (NIC timestamps) |
| Throughput, per-flow and aggregate | neper / iperf3 |
| Flow completion time (mean, p99, p99.9) | neper `tcp_rr`, incast generator |
| Loss, retransmission, reordering | `nstat` TCP counters, receiver packet capture |
| Queue occupancy | `tc -s qdisc` on router, sampled |
| Fairness | Jain's index over per-flow throughput |

### 6.6 Controls for validity

- **Tuning and environment.** `tune.sh` and `verify.sh` run on every node, every
  session. The unloaded TCP RTT baseline is recorded per session, and numbers
  from different sessions are compared only if their baselines match.
- **Host artifacts.** Receive-queue and CPU pinning are fixed so per-flow
  throughput differences come from congestion control, not from flows sharing
  receive cores. The 8-flow imbalance (16.5 vs 8.3 Gbps per flow) is our test
  case for this.
- **Receiver configuration** is the same for all algorithms (quick ACKs on),
  so no algorithm is disadvantaged by delayed ACKs.
- **Stated simplifications:** configured hop counts instead of TTL echo;
  software timestamps in the control loop (with hardware ground truth measured
  alongside); MSwift's median window capped where BPF requires it.

## 7. Progress and Next Steps

**Done:**
- Platform selected; per-session setup and reservation workflow documented.
- Hardware timestamping validated at all three levels, including on TCP data.
- Host versus network delay decomposition measured.
- Session tuning and verification scripts written and debugged (fixed a bug that
  silently failed to load BBR and Vegas).
- BPF prototype confirms the per-ACK hook, RTT samples, enforced pacing, and
  sub-packet-per-RTT sending on the stock kernel.

**Next milestones:**

| By | Milestone |
|---|---|
| Progress Report 2 | Swift core (AIMD, fractional cwnd, target scaling) in BPF with ring-buffer telemetry. neper and incast generator running. Scenario B results vs Cubic, DCTCP, Vegas (RQ1, reproduction). |
| Progress Report 2 | Spraying topology built and passing all three verification checks. |
| Final | Incast sweeps (RQ3). Fabric/endpoint split via TCP header option (RQ2). MSwift median filter and q sweep. |
| Stretch | NIC timestamps fed into the control loop; compare control quality with software timestamps. |

**Artifacts:** scripts in `verification/scripts/` (`tune.sh`, `verify.sh`,
`hwts_pingpong.c`, `cc_probe.bpf.c`); raw data and session summaries in
`results/2026-10-11/` and `verification/`.
