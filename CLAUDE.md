# GSwifT

Reproducing Google's Swift delay-based congestion control on commodity Linux,
as an eBPF `struct_ops` TCP congestion control module.

Course project. Team of 5. Georgia Tech, Fall 2026.

## Research question

Swift achieves low latency and high throughput by precisely measuring RTT and
separating delay caused by the network fabric from delay caused by the endpoint.
Google's implementation depends on NIC hardware timestamps, an OS-bypass
transport stack (Pony Express / Snap), and fine-grained pacing. We ask how much
of that benefit survives on commodity Linux.

Three capabilities we are testing for portability:

1. Measuring delay accurately enough to control it, using software rather than
   NIC timestamps.
2. Distinguishing network congestion from receiver-side congestion.
3. Handling large incasts by sending less than one packet per round trip.

## Key papers

- **Swift** (Kumar et al., SIGCOMM 2020). The core design. Target-delay AIMD,
  decrease at most once per RTT, delay split into fabric and endpoint
  components, `cwnd` allowed below 1 with pacing, target scaled by topology
  (hop count) and load (1/sqrt(cwnd)).
- **MSwift** (Gerstein, Silberstein, Keslassy, arXiv 2025). Shows Swift's
  throughput collapses as 1/sqrt(q) when packets are sprayed across paths and a
  fraction q hit a congested path. Fixes it by reacting to the median of the
  last H delay samples, H = max(cwnd/2, 1), plus reordering tolerance.
- **Falcon** (Singhvi et al., SIGCOMM 2025). Google's hardware transport. Uses a
  Swift variant with two congestion windows (fabric and NIC). Shows where the
  design ends up with full hardware support.

## Hardware environment

CloudLab, Clemson cluster, `small-lan` profile.

| | |
|---|---|
| Node type | r6525 (AMD EPYC Milan, 64 core, 256 GB) |
| OS image | UBUNTU24-64-STD |
| Kernel | 6.8.0-138-generic |
| Project | gt-8803-dns |

### Interfaces (verified Oct 10 2026)

| Interface | Card | PHC | Network | Role |
|---|---|---|---|---|
| `enp129s0f0np0` | ConnectX-6 Dx (MT2892) | 2 | 10.10.1.x | **experiment** |
| `eno12399np0` | ConnectX-5 (MT27800) | 0 | public | control |

Both cards support `HWTSTAMP_FILTER_ALL`, meaning they timestamp every received
packet rather than only PTP packets. This is the hard requirement for Swift:
ConnectX-4 and newer only. Experiment link is 100 Gbps over direct attach
copper.

### Verified toolchain

- `CONFIG_BPF_SYSCALL`, `CONFIG_BPF_JIT`, `CONFIG_BPF_JIT_ALWAYS_ON`
- `CONFIG_DEBUG_INFO_BTF`, `CONFIG_DEBUG_INFO_BTF_MODULES`
- `/sys/kernel/btf/vmlinux` present
- `bpftool feature probe`: struct_ops available as both program type and map type

So a BPF struct_ops congestion control module will load on the stock image. No
custom kernel needed.

Note: `tcp_available_congestion_control` shows only `reno cubic` until you
`modprobe tcp_dctcp tcp_bbr tcp_vegas`. They are built as loadable modules.

## Measured baselines (Oct 10 2026, 2 x r6525)

Unloaded ICMP RTT, node0 to node1, 100 packets:

| | untuned | tuned (both nodes) |
|---|---|---|
| min | 116 µs | **47 µs** |
| avg | 395 µs | 119 µs |
| max | 518 µs | 254 µs |
| mdev | 78 µs | 35 µs |

PTP hardware-timestamped path delay on the same idle link: **1338 ns one-way**,
stable within a few ns over 60 s. Clock offset converged to within ±14 ns
(servo state s2).

**Implied breakdown of the 47 µs tuned RTT:** roughly 2.7 µs of actual network
(2 x 1.34 µs wire) and roughly 44 µs of host stack across both ends. About 94%
of observed application-level delay is host software, not the network.

This is the central finding so far and directly motivates the project. Caveats
to state honestly: ICMP is not TCP, and PTP path delay is a filtered estimate
over an idle link, so treat this as order-of-magnitude rather than exact.

## Critical: tuning does not persist

Performance governor, C-state, and interrupt coalescing settings are lost on
every new experiment. Run `scripts/tune.sh` on **every node, every session**, or
results will not be comparable across runs. Untuned RTT is 2.5x higher on the
minimum and 3.3x higher on the average.

## CloudLab workflow

Nodes are wiped when the experiment expires. Nothing persists. Therefore:

- Code lives in this repo, not on the nodes.
- Develop locally (network namespaces in a Linux VM work for most of it).
- On the node: clone, run `scripts/tune.sh`, build, run, write results to
  `/mydata`.
- `scp` results back or commit them before the experiment expires.

Default experiment duration is limited and extendable from the experiment page.
Clemson r6525 nodes are in demand, so reserve in advance for real runs rather
than hoping for on-demand availability.

## Implementation plan

### Core module

Swift's Algorithm 1 as a BPF struct_ops congestion control module, via the
`cong_control` hook. Start from the kernel's `bpf_dctcp` selftest as a working
reference.

Pieces, roughly in order:

1. Target-delay AIMD. Increase `cwnd` by `ai/cwnd` per ACK when delay is below
   target; multiplicative decrease scaled by how far delay exceeds target,
   limited to once per RTT.
2. Fractional `cwnd` below 1, enforced through `sk_pacing_rate` as an
   inter-packet delay of RTT/cwnd. Needed for large incast.
3. Flow-based target scaling, target proportional to 1/sqrt(cwnd).
4. Topology-based scaling via hop count. TTL echoing is awkward in the kernel;
   a configured per-destination hop count is an acceptable simplification.
5. (Stretch) Fabric vs endpoint split. Requires the receiver to report its own
   queuing delay back to the sender, so it touches both ends of the connection.

### Known constraints

- Linux `cwnd` is an integer with a floor of 1. Keep a fractional `cwnd` in the
  module's own state and drive pacing from it.
- A BPF congestion control module has only about 100 bytes of per-socket private
  state. A median window of cwnd/2 samples (for MSwift) will not fit. Either cap
  H at a small fixed size (16, sorted insertion is cheap) or keep the ring
  buffer in a BPF map keyed by socket cookie.
- Delayed ACKs inflate RTT samples. Disable on the receiver (`TCP_QUICKACK`) or
  account for them.
- Hardware timestamps are not exposed to struct_ops congestion control. Plumbing
  them into the control loop is nontrivial; the fallback is to measure both ways
  in a userspace harness and quantify the gap.

### MSwift evaluation

Median filter over the last H delay samples plus reordering tolerance (decrease
only after several consecutive delayed packets rather than the first).

Packet spraying, in increasing order of realism:

1. nftables/iptables `statistic` random matching sends a fraction q of packets
   down a longer, congested path. Directly instantiates the MSwift model where
   one path out of n is congested, so q = 1/n.
2. A tc-eBPF egress program that rewrites the IPv6 flow label per packet,
   combined with multipath routing hashing on the flow label. This is how real
   sprayers and Google's PLB work.

Verifying spraying actually works, three independent checks:

1. **Counts.** Per-path packet and byte counters vs the intended split.
2. **Reordering.** Fraction of out-of-order segments and reorder distance at the
   receiver; cross-check `nstat` SACK reordering counters.
3. **Delay bimodality.** Histogram of per-ACK delay samples should show two
   modes. Cleanest evidence, because it validates the exact assumption MSwift's
   analysis rests on.

Target result: sweep q and check whether baseline Swift's throughput falls
roughly as 1/sqrt(q), then show the median filter flattens that curve.

## Evaluation plan

Baselines: Cubic and DCTCP, both in-tree. Vegas as the other delay-based
reference.

Traffic generators:
- **neper** (`tcp_rr`, `tcp_stream`) for RPC-style latency and bulk throughput.
  Google's own, best match for Swift's short-RPC tail latency metric.
- **iperf3** for simple bulk flows.
- A custom incast client/server: N senders firing simultaneously at one
  receiver, N swept. Off-the-shelf tools do not synchronize fan-in.

Conditions to sweep: fan-in degree (10 to 500), offered load (20% to 100%),
bottleneck bandwidth, base RTT (short and multi-hop), buffer size, target delay,
and competing traffic (Swift alone, vs Cubic, vs DCTCP).

Metrics: RTT and cwnd traces exported from the module through a ring buffer;
flow completion times from the generator; loss and reordering from `nstat`;
queue occupancy from `tc -s qdisc`; fairness as Jain's index.

### Scale notes

- 2 nodes gives a real link but **no congestion**. Fine for development and
  timestamp work, useless for the actual congestion experiments.
- Incast needs many senders. Clemson has 32 r6525s. Many flows per sender
  stretches a smaller allocation further, but real machines give more realistic
  host-side behavior.
- 100 Gbps is fast enough that a single kernel TCP flow will not saturate it.
  Expect to need many parallel flows or `tc` shaping to create a real
  bottleneck. This shapes experiment design.

## Conventions

- No em dashes in written output.
- Results go under `results/<date>/`, never left only on a node.
- Every experiment session records: node type, kernel, interface, whether
  `tune.sh` was applied, and the unloaded RTT baseline for that session.
  Numbers from different sessions are not comparable without this.
