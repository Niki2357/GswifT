# Running the GSwifT experiments

Two nodes on the same LAN: **node0** sends (Swift loaded here, scripts run
here), **node1** receives (emulated bottleneck on its ingress). Any CloudLab
type with a ConnectX-4+ NIC works; every session records which one it was.

## 0. Reserving nodes

- Pick the **UBUNTU24-64-STD** image (kernel 6.8). On Ubuntu 22.04 (kernel
  5.15) Swift cannot pace below one packet; you would have to install
  `linux-generic-hwe-22.04` and reboot (about 15 minutes).
- Prefer **c6525-25g** again so numbers compare with the 2026-11-09 session.
- Add your laptop's public key under CloudLab, Manage SSH Keys, before the
  experiment starts. Then run `ssh-add ~/.ssh/id_ed25519` locally so
  `ssh -A` can forward it.

## 1. Each node, every session

```bash
git clone https://github.com/Niki2357/GswifT.git ~/GswifT
cd ~/GswifT && ./scripts/setup_node.sh   # deps, build, unit tests, tune.sh, sysctls
```

Log in to node0 with agent forwarding (`ssh -A user@node0...`) so it can ssh
to node1 without a password. After any reboot, rerun `./scripts/tune_session.sh`
on both nodes (tuning and sysctls do not persist).

## 2. On node0

```bash
./scripts/smoke_test.sh      # ~2 min, must print ALL PASS
./scripts/run_all.sh         # ~30 min: session info + E0..E4, then a .tgz bundle
```

Individual experiments, each tunable with env vars at the top of the script:

| Script | Question | Sweeps |
|---|---|---|
| `run_e0_noise.sh` | RQ1: RTT noise with no queue | offered load |
| `run_e1_compare.sh` | Swift vs Cubic/DCTCP/Vegas/BBR | algorithm x {1, 8} flows |
| `run_e2_target.sh` | RQ1: lowest usable target | base target 25-500 us |
| `run_e3_incast.sh` | RQ3: fractional cwnd under incast | fan-in 16-512, frac on/off |
| `run_e4_spray.sh` | MSwift: median filter vs spraying | q = 0-0.5 |

Results land in `/mydata/gswift/<date>/`. Copy them back before the
experiment expires:

```bash
scp user@node0:/mydata/gswift/results-<date>.tgz . && tar -xzf results-<date>.tgz -C results/
python3 -m analysis.analyze results/<date>    # summary.md, tables/, figs/
```

## Layout

| Path | What |
|---|---|
| `src/swift_core.h` | Swift + MSwift algorithm, pure C, unit-tested (`make -C tests`) |
| `src/swift.bpf.c` | struct_ops adapter: cwnd, pacing, RTO/recovery hooks, telemetry |
| `src/swiftctl*.c` | loader: parameters, attach, per-ACK CSV trace, counters |
| `tools/incast/` | synchronized incast + RPC latency probe |
| `scripts/` | setup, bottleneck, spraying, queue monitor, experiments |
| `analysis/` | loaders, metrics, figures, `analyze.py` (`pytest analysis/tests`) |

## Results in the repo

`results/<date>/` holds everything except the per-ACK `trace.csv` files,
which are hundreds of MB per session and are excluded by `.gitignore`. Keep
the session bundle (`results-<date>.tgz` from the node) somewhere shared;
the analysis needs the traces for the Swift-specific columns and the trace
figures. `summary.md`, `tables/*.csv` and `figs/*.png` are committed.

## Next session plan

Status after 2026-11-10 (logs: `results/2026-11-09/SESSION.md`,
`results/2026-11-10/SESSION.md`; report: `reports/progress2/progress_report_2.md`).
The Nov 10 plan (repeats, MSwift-focused E4, pacing fix) is done. Next, from
report section 7:

1. **Adaptive target.** Set the target relative to a measured RTT floor (for
   example a windowed low percentile) and test it in both host states seen on
   Nov 10 (Swift at 100 µs gave 92% at 11:19 and 43% at 13:30).
2. **Lone-packet recovery.** Lower rto_min from the module
   (`bpf_setsockopt(TCP_BPF_RTO_MIN)`) and keep two packets in flight below one
   packet per RTT; rerun E3 at N = 64, 128 against `rto_min 5ms` Cubic/DCTCP.
3. **MSwift vs its model.** Spray per wire packet (tc-BPF after
   segmentation) and sweep the slow-path delay relative to the target.

Node notes: if the image is Ubuntu 20.04, see `results/2026-11-10/SESSION.md`
(mainline 6.8 kernel, decompress modules, tc from iproute2 6.8 for
`drop_batch`). Always run E1 and E2 interleaved in one block when comparing
targets: Swift's result at a 100 µs target drifts within a session.
