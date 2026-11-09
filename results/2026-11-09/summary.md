# Results summary: 2026-11-09

## Session nodes.txt

```
# sender
hostname=amd108.utah.cloudlab.us
node_type=c6525-25g
kernel=6.8.0-138-generic
os=Ubuntu 22.04.2 LTS
cpu=AMD EPYC 7302P 16-Core Processor
iface=enp65s0f0np0
nic=driver: mlx5_core firmware-version: 16.28.4512 (DEL0000000015) 
pci=Ethernet controller: Mellanox Technologies MT27800 Family [ConnectX-5]
speed=25000Mb/s mtu=1500
governor=
coalesce=Adaptive RX: off  TX: off rx-usecs: 0 
hwts_rx_all=yes
cc_available=reno cubic dctcp bbr vegas
libbpf=1.4.5

# receiver
hostname=amd111.utah.cloudlab.us
node_type=c6525-25g
kernel=6.8.0-138-generic
os=Ubuntu 22.04.2 LTS
cpu=AMD EPYC 7302P 16-Core Processor
iface=enp65s0f0np0
nic=driver: mlx5_core firmware-version: 16.28.4512 (DEL0000000015) 
pci=Ethernet controller: Mellanox Technologies MT27800 Family [ConnectX-5]
speed=25000Mb/s mtu=1500
governor=
coalesce=Adaptive RX: off  TX: off rx-usecs: 0 
hwts_rx_all=yes
cc_available=reno cubic dctcp bbr vegas
libbpf=1.4.5
```

## Session baseline.txt

```
tcp_rr_64B_us n=20000 p1=40.7 p50=42.2 p99=55.3 p99.9=80.7
```

## E0: RTT noise floor (no bottleneck)

| load | goodput_gbps | samples | rtt_us_min | rtt_us_p50 | rtt_us_p99 | rtt_us_p999 | excess_us_p50 | excess_us_p99 |
|---|---|---|---|---|---|---|---|---|
| 1G | 0.6 | 39187 | 21 | 56 | 82 | 88 | 20 | 46 |
| 5G | 5 | 164479 | 21 | 118 | 196 | 208 | 68 | 146 |
| max | 16.3 | 463426 | 29 | 340 | 477 | 534 | 149 | 286 |

## E1: algorithm comparison at the bottleneck

| variant | flows | goodput_gbps | util_pct | queue_us_p50 | queue_us_p99 | rpc_us_p50 | rpc_us_p99 | retrans_pct | jain | rtt_us_p50 | target_us | decr_per_s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bbr | 1 | 9.47 | 94.7 | 109 | 327 | 190 | 407 | 6.08e-06 | 1 |  |  |  |
| cubic | 1 | 8.84 | 88.4 | 0 | 54.7 | 57.2 | 146 | 0 | 1 |  |  |  |
| dctcp | 1 | 9.23 | 92.3 | 0 | 66.6 | 72.5 | 169 | 0.00205 | 1 |  |  |  |
| swift | 1 | 9.25 | 92.5 | 0 | 36.4 | 67.2 | 134 | 0 | 1 | 72 | 100 | 2.19e+03 |
| vegas | 1 | 8.23 | 82.3 | 0 | 0 | 49.7 | 81.4 | 1.39e-05 | 1 |  |  |  |
| bbr | 8 | 9.5 | 95 | 1.74e+03 | 2.02e+03 | 1.82e+03 | 2.15e+03 | 0 | 1 |  |  |  |
| cubic | 8 | 5.18 | 51.8 | 0 | 191 | 148 | 417 | 0.00258 | 1 |  |  |  |
| dctcp | 8 | 9.46 | 94.6 | 13.3 | 119 | 135 | 264 | 6e-06 | 1 |  |  |  |
| swift | 8 | 2.19 | 21.9 | 0 | 1.21 | 89.4 | 245 | 0.00282 | 1 | 81 | 100 | 7.94e+03 |
| vegas | 8 | 3.68 | 36.8 | 0 | 7.36 | 152 | 384 | 0.00328 | 1 |  |  |  |

## E2: base-target sweep (8 flows)

| base_target_us | goodput_gbps | util_pct | rtt_us_p50 | rtt_us_p99 | inferred_queue_us_p50 | queue_us_p50 | queue_us_p99 | rpc_us_p99 | decr_per_s | retrans_pct |
|---|---|---|---|---|---|---|---|---|---|---|
| 25 | 0.00833 | 0.0833 | 26 | 49 | -14.7 | 0 | 0 | 58.7 | 1e+03 | 0.000686 |
| 50 | 0.22 | 2.2 | 49 | 128 | 8.3 | 0 | 0 | 181 | 8.7e+03 | 0.0025 |
| 75 | 1.54 | 15.4 | 72 | 165 | 31.3 | 0 | 1.21 | 278 | 1.74e+04 | 0.00078 |
| 100 | 5.27 | 52.7 | 80 | 185 | 39.3 | 0 | 16.1 | 199 | 1.73e+04 | 0 |
| 150 | 3.34 | 33.4 | 117 | 255 | 76.3 | 0 | 96.7 | 2.36e+03 | 5.86e+03 | 0.00788 |
| 200 | 5.07 | 50.7 | 165 | 288 | 124 | 0 | 173 | 785 | 8.12e+03 | 0.00674 |
| 300 | 9.29 | 92.9 | 260 | 369 | 219 | 182 | 277 | 466 | 9.67e+03 | 0 |
| 500 | 9.54 | 95.4 | 466 | 570 | 425 | 392 | 469 | 717 | 6.69e+03 | 0 |

## E3: incast fan-in sweep

| variant | fanin | qct_us_p50 | qct_us_p99 | fct_us_p99 | retrans_pct | timeouts | drops | queue_us_p99 | frac_ack_pct |
|---|---|---|---|---|---|---|---|---|---|
| cubic | 16 | 1.08e+03 | 1.36e+03 | 1.31e+03 | 0 | 0 | 0 | 0 |  |
| dctcp | 16 | 860 | 1.07e+03 | 1.01e+03 | 0.00126 | 0 | 0 | 39.4 |  |
| swift | 16 | 5.31e+03 | 8.19e+03 | 6.77e+03 | 0.00126 | 0 | 0 | 0 | 5.29 |
| swift-nofrac | 16 | 4.02e+03 | 5.43e+03 | 5.26e+03 | 0 | 0 | 0 | 0 | 0 |
| cubic | 32 | 1.05e+04 | 1.13e+04 | 1.13e+04 | 0 | 0 | 0 | 70.2 |  |
| dctcp | 32 | 1.01e+04 | 1.1e+04 | 1.09e+04 | 0 | 0 | 0 | 78.3 |  |
| swift | 32 | 1.02e+04 | 1.61e+04 | 1.16e+04 | 0 | 0 | 0 | 97.2 | 10.5 |
| swift-nofrac | 32 | 9.19e+03 | 1.08e+04 | 1.03e+04 | 0.0019 | 0 | 0 | 13.5 | 0 |
| cubic | 64 | 2.16e+05 | 2.23e+05 | 2.2e+05 | 3.19 | 419 | 1.01e+04 | 117 |  |
| dctcp | 64 | 2.14e+05 | 2.23e+05 | 2.15e+05 | 3.45 | 328 | 1e+04 | 118 |  |
| swift | 64 | 2.06e+05 | 2.1e+05 | 2.06e+05 | 2.68 | 0 | 5.63e+03 | 13.7 | 16.2 |
| swift-nofrac | 64 | 2.17e+04 | 2.28e+04 | 2.26e+04 | 0.0379 | 0 | 39 | 67.9 | 0 |
| cubic | 128 | 2.29e+05 | 6.31e+05 | 2.31e+05 | 7.51 | 6.38e+03 | 5.05e+04 | 121 |  |
| dctcp | 128 | 2.27e+05 | 6.3e+05 | 2.3e+05 | 8.27 | 6.14e+03 | 5.09e+04 | 120 |  |
| swift | 128 | 2.13e+05 | 2.47e+05 | 2.13e+05 | 5.91 | 149 | 2.36e+04 | 76.3 | 21.6 |
| swift-nofrac | 128 | 2.22e+05 | 2.25e+05 | 2.25e+05 | 1.04 | 12 | 9.57e+03 | 91.1 | 0 |
| cubic | 256 | 1.47e+06 | 1.48e+06 | 1.47e+06 | 10.8 | 3.28e+04 | 1.41e+05 | 117 |  |
| dctcp | 256 | 1.47e+06 | 1.48e+06 | 1.47e+06 | 11.9 | 3.31e+04 | 1.4e+05 | 117 |  |
| swift | 256 | 6.91e+05 | 9.48e+05 | 6.77e+05 | 9.36 | 8.32e+03 | 6.95e+04 | 72.7 | 20.2 |
| swift-nofrac | 256 | 4.37e+05 | 6.13e+05 | 4.4e+05 | 2.27 | 6.27e+03 | 3.91e+04 | 111 | 0 |
| cubic | 512 | 6.37e+06 | 6.41e+06 | 6.4e+06 | 13.6 | 4.02e+04 | 1.17e+05 | 110 |  |
| dctcp | 512 | 6.38e+06 | 6.41e+06 | 6.4e+06 | 14 | 4.08e+04 | 1.15e+05 | 107 |  |
| swift | 512 | 1.76e+06 | 3.27e+06 | 1.8e+06 | 15.5 | 2.13e+04 | 7.1e+04 | 23.9 | 16.7 |
| swift-nofrac | 512 | 1.66e+06 | 2.84e+06 | 1.68e+06 | 4.74 | 1.75e+04 | 4.75e+04 | 112 | 0 |

## E4: spraying with one slow path

| variant | q | q_measured | goodput_gbps | util_pct | retrans_pct | reorder_events | fast_recoveries | decr_per_s | rtt_us_p50 |
|---|---|---|---|---|---|---|---|---|---|
| cubic | 0 | 0 | 3.81 | 38.1 | 0.000479 | 0 | 0 |  |  |
| cubic | 0.05 | 0.0405 | 1.66 | 16.6 | 2.3 | 4.68e+04 | 8.71e+04 |  |  |
| cubic | 0.1 | 0.0912 | 1.18 | 11.8 | 3.32 | 9.27e+04 | 1.35e+05 |  |  |
| cubic | 0.25 | 0.236 | 1.19 | 11.9 | 4.85 | 1.94e+05 | 2.06e+05 |  |  |
| cubic | 0.5 | 0.46 | 0.846 | 8.46 | 3.61 | 2.08e+05 | 1.76e+05 |  |  |
| mswift | 0 | 0 | 1.64 | 16.4 | 0.00571 | 0 | 5 | 9.9e+03 | 96 |
| mswift | 0.05 | 0.0162 | 1.39 | 13.9 | 2.93 | 1.41e+04 | 2.85e+04 | 5.04e+03 | 85 |
| mswift | 0.1 | 0.0501 | 1.93 | 19.3 | 5.64 | 5.19e+04 | 9.27e+04 | 1.06e+04 | 89 |
| mswift | 0.25 | 0.172 | 1.64 | 16.4 | 5.96 | 1.73e+05 | 1.12e+05 | 9.4e+03 | 63 |
| mswift | 0.5 | 0.304 | 0.121 | 1.21 | 0.472 | 1.78e+04 | 5.32e+03 | 3.59e+03 | 32 |
| swift | 0 | 0 | 0.487 | 4.87 | 0.00869 | 0 | 9 | 7.01e+03 | 90 |
| swift | 0.05 | 0.0261 | 1.02 | 10.2 | 3.16 | 1.62e+04 | 4.26e+04 | 9.49e+03 | 88 |
| swift | 0.1 | 0.0669 | 2.29 | 22.9 | 5.12 | 7.02e+04 | 1.28e+05 | 2.12e+04 | 88 |
| swift | 0.25 | 0.199 | 1.14 | 11.4 | 4.87 | 1.26e+05 | 8.04e+04 | 1.17e+04 | 65 |
| swift | 0.5 | 0.305 | 0.114 | 1.14 | 0.478 | 1.94e+04 | 6.83e+03 | 3.57e+03 | 32 |
