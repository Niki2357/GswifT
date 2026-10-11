# CloudLab session setup

Everything you need to do at the start of each experiment session, and what we
have already verified about the hardware.

## Starting an experiment

CloudLab portal, Experiments -> Start Experiment.

**Profile:** `small-lan`

**Parameterize:**

| Field | Value |
|---|---|
| Number of Nodes | 2 (more for incast runs) |
| Select OS image | Ubuntu 24.04 |
| Optional physical node type | `r6525` |
| Use XEN VMs | unchecked |
| Start X11 VNC | unchecked |

**Advanced:**

| Field | Value |
|---|---|
| Link Speed | Any |
| Best Effort | unchecked |
| No Interswitch Links | unchecked |
| Temporary Filesystem Size | 0 |
| Temp Filesystem Max Space | **checked** |
| Temporary Filesystem Mount Point | `/mydata` |
| Force use of exclusive VMs | **unchecked** |

**Finalize:** cluster must be **Clemson** (that is where r6525 lives).

r6525 is often fully booked. For real experiment runs, request a reservation in
advance (Experiments -> Reservations) rather than hoping for on-demand
availability, or schedule the experiment to start at a future time.

## Per-session checklist

On **every node**, every session:

```bash
git clone <repo> && cd <repo>
./tune.sh          # REQUIRED, see below
./verify.sh        # captures environment state to /mydata/verify/<hostname>
```

`tune.sh` is not optional. The performance governor, C-state, and interrupt
coalescing settings are reset on every new allocation, and without them the
unloaded RTT is 2.5x higher on the minimum and 3.3x higher on the average.
Results from a tuned and an untuned session are not comparable.

## Verified hardware facts (Oct 10 2026)

### Node

r6525, Clemson. AMD EPYC Milan, 64 core, 256 GB RAM. Ubuntu 24.04, kernel
6.8.0-138-generic.

### Interfaces

| Interface | Card | PCI | PHC | Address | Role |
|---|---|---|---|---|---|
| `enp129s0f0np0` | ConnectX-6 Dx (MT2892) | 81:00.0 | 2 | 10.10.1.x | **experiment** |
| `enp129s0f1np1` | ConnectX-6 Dx (MT2892) | 81:00.1 | 3 | down | unused |
| `eno12399np0` | ConnectX-5 (MT27800) | 63:00.0 | 0 | public | control |
| `eno12409np1` | ConnectX-5 (MT27800) | 63:00.1 | 1 | down | unused |

Interface names may differ on other nodes. Always identify the experiment
interface by its 10.10.1.x address, not by name. The rspec calls it `eth1`, but
Ubuntu uses predictable naming.

### Hardware timestamping

Both the ConnectX-6 Dx and the ConnectX-5 report:

```
Capabilities:
    hardware-transmit
    hardware-receive
    hardware-raw-clock
Hardware Receive Filter Modes:
    none
    all          <-- HWTSTAMP_FILTER_ALL
```

`all` is what matters. It means the card timestamps every received packet, not
only PTP packets. Most NICs advertise hardware timestamping but implement only
the PTP filter modes, which is useless for Swift. ConnectX-4 and newer are the
only cards that support this.

### Link

100 Gbps, direct attach copper, link detected, 0% loss.

### eBPF support

- BTF present at `/sys/kernel/btf/vmlinux` (6 MB)
- `CONFIG_BPF_SYSCALL`, `CONFIG_BPF_JIT`, `CONFIG_BPF_JIT_ALWAYS_ON`
- `CONFIG_DEBUG_INFO_BTF`, `CONFIG_DEBUG_INFO_BTF_MODULES`
- struct_ops available as both program type and map type

A BPF struct_ops congestion control module will load on the stock image.

Note: `tcp_available_congestion_control` initially shows only `reno cubic`.
DCTCP, BBR and Vegas ship as loadable modules; `tune.sh` modprobes them.

## Measured baselines

### Unloaded ICMP RTT (node0 to node1, 100 packets)

| | untuned | tuned (both nodes) |
|---|---|---|
| min | 116 µs | **47 µs** |
| avg | 395 µs | 119 µs |
| max | 518 µs | 254 µs |
| mdev | 78 µs | 35 µs |

### PTP hardware-timestamped path delay

Same idle link, `ptp4l -H` with node0 as master and node1 as slave:

- **Path delay 1338 ns one-way**, stable within a few ns over 60 s
- Clock offset converged from -362 ms to within ±14 ns in about 2 s
- Servo reached locked state (s2)

### What this means

Roughly 2.7 µs of the 47 µs tuned RTT is actual network (2 x 1.34 µs wire).
The remaining ~44 µs is host stack across both ends. About **94% of observed
application-level delay is host software, not the network.**

Caveats to state when reporting this: ICMP is not TCP, and PTP path delay is a
filtered estimate over an idle link. Treat as order-of-magnitude.

## Getting results off the node

Nodes are wiped when the experiment expires. Before then:

```bash
# from your local machine
scp -r <user>@<node-hostname>:/mydata/verify ./results/<date>/
```

Hostnames look like `clnode311.clemson.cloudlab.us` and appear on the experiment
status page. If scp fails with "Permission denied (publickey)", your local SSH
key is not registered with CloudLab. Add it under your account's SSH keys page
and wait a minute for it to propagate to running nodes.

## Reproducing the PTP test

Two terminals. On node0 (master):

```bash
sudo ptp4l -i $IFACE -m -H 2>&1 | tee ptp4l-master.txt
```

Wait for "assuming the grand master role", then on node1 (slave):

```bash
sudo ptp4l -i $IFACE -m -H -s 2>&1 | tee ptp4l-slave.txt
```

Let it run 60 s, then Ctrl-C both. Success looks like the slave transitioning
LISTENING -> UNCALIBRATED -> SLAVE and printing `master offset` lines that
converge toward zero with state `s2`.

Always use `tee` so the log is a file, not just scrollback.
