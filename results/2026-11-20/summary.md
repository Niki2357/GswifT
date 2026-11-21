# Results summary: 2026-11-20

## Session nodes.txt

```
# sender
hostname=amd128.utah.cloudlab.us
node_type=c6525-25g
kernel=6.8.0-149-generic
os=Ubuntu 22.04.2 LTS
cpu=AMD EPYC 7302P 16-Core Processor
iface=enp65s0f0np0
nic=driver: mlx5_core firmware-version: 16.28.4512 (DEL0000000015) 
pci=Ethernet controller: Mellanox Technologies MT27800 Family [ConnectX-5]
speed=25000Mb/s mtu=1500
governor=performance
coalesce=Adaptive RX: off  TX: off rx-usecs: 0 
hwts_rx_all=yes
cc_available=reno cubic dctcp bbr vegas
libbpf=1.4.5

# receiver
hostname=amd155.utah.cloudlab.us
node_type=c6525-25g
kernel=6.8.0-149-generic
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
tcp_rr_64B_us n=20000 p1=36.6 p50=38.0 p99=50.9 p99.9=65.9
```

## E0: RTT noise floor (no bottleneck)

| load | goodput_gbps | samples | rtt_us_min | rtt_us_p50 | rtt_us_p99 | rtt_us_p999 | excess_us_p50 | excess_us_p99 |
|---|---|---|---|---|---|---|---|---|
| 1G | 0.6 | 37937 | 25 | 60 | 101 | 122 | 15 | 56 |
| 5G | 5 | 165382 | 22 | 130 | 225 | 260 | 80 | 175 |
| max | 15.3 | 442905 | 32 | 354 | 475 | 501 | 151 | 272 |

## E1: algorithm comparison at the bottleneck

Median (min-max) over n repeats.

| noise | flows | variant | n | goodput_gbps | util_pct | queue_us_p99 | rpc_us_p99 | retrans_pct | rtt_us_p50 | decr_per_s | jain |
|---|---|---|---|---|---|---|---|---|---|---|---|
| none | 8 | cubic | 5 | 8.04 (7.51-8.14) | 80.4 (75.1-81.4) | 190 (178-221) | 329 (318-334) | 9.41e-06 (0-9.86e-06) |  |  | 1 (1-1) |
| none | 8 | dctcp | 5 | 9.5 (9.27-9.53) | 95 (92.7-95.3) | 133 (128-155) | 286 (238-289) | 0 (0-8.06e-06) |  |  | 1 (1-1) |

<details><summary>Every run</summary>

| run | rep | variant | noise | flows | goodput_gbps | util_pct | queue_us_p50 | queue_us_p99 | rpc_us_p50 | rpc_us_p99 | retrans_pct | jain | rtt_us_p50 | target_us | decr_per_s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cubic-n8-r1 | 1 | cubic | none | 8 | 8.05 | 80.5 | 0 | 221 | 144 | 322 | 0 | 1 |  |  |  |
| cubic-n8-r2 | 2 | cubic | none | 8 | 7.95 | 79.5 | 0 | 190 | 147 | 334 | 9.48e-06 | 1 |  |  |  |
| cubic-n8-r3 | 3 | cubic | none | 8 | 8.04 | 80.4 | 0 | 202 | 146 | 333 | 9.41e-06 | 1 |  |  |  |
| cubic-n8-r4 | 4 | cubic | none | 8 | 7.51 | 75.1 | 0 | 178 | 149 | 329 | 9.86e-06 | 1 |  |  |  |
| cubic-n8-r5 | 5 | cubic | none | 8 | 8.14 | 81.4 | 0 | 184 | 143 | 318 | 9.28e-06 | 1 |  |  |  |
| dctcp-n8-r1 | 1 | dctcp | none | 8 | 9.53 | 95.3 | 4.84 | 129 | 164 | 250 | 0 | 1 |  |  |  |
| dctcp-n8-r2 | 2 | dctcp | none | 8 | 9.38 | 93.8 | 0 | 153 | 174 | 286 | 8.04e-06 | 1 |  |  |  |
| dctcp-n8-r3 | 3 | dctcp | none | 8 | 9.5 | 95 | 1.21 | 133 | 168 | 289 | 0 | 1 |  |  |  |
| dctcp-n8-r4 | 4 | dctcp | none | 8 | 9.53 | 95.3 | 4.84 | 128 | 165 | 238 | 0 | 1 |  |  |  |
| dctcp-n8-r5 | 5 | dctcp | none | 8 | 9.27 | 92.7 | 0 | 155 | 180 | 286 | 8.06e-06 | 1 |  |  |  |

</details>

## E2: base-target sweep (8 flows)

Median (min-max) over n repeats.

| noise | base_target_us | n | util_pct | goodput_gbps | rtt_us_p50 | rtt_us_p99 | queue_us_p50 | queue_us_p99 | rpc_us_p99 | decr_per_s |
|---|---|---|---|---|---|---|---|---|---|---|
| none | 100 | 5 | 7.88 (7.03-16.1) | 0.788 (0.703-1.61) | 98 (96-99) | 193 (181-215) | 0 (0-0) | 0 (0-30.3) | 181 (157-189) | 9.05e+03 (8.43e+03-1.14e+04) |
| none | 150 | 5 | 81.2 (29.9-93.3) | 8.12 (2.99-9.33) | 133 (123-135) | 227 (224-243) | 9.69 (0-35.1) | 113 (93.8-116) | 266 (224-287) | 1.98e+04 (5.71e+03-2.44e+04) |
| none | 200 | 5 | 93.9 (45.7-95.4) | 9.39 (4.57-9.54) | 184 (168-188) | 262 (254-296) | 114 (0-128) | 169 (167-181) | 298 (282-384) | 1.91e+04 (7.96e+03-2.08e+04) |
| none | 300 | 5 | 95.4 (95.2-95.6) | 9.54 (9.52-9.56) | 286 (283-286) | 356 (356-360) | 225 (223-226) | 270 (267-277) | 401 (396-409) | 1.37e+04 (1.32e+04-1.38e+04) |

<details><summary>Every run</summary>

| run | rep | noise | base_target_us | goodput_gbps | util_pct | rtt_us_p50 | rtt_us_p99 | inferred_queue_us_p50 | queue_us_p50 | queue_us_p99 | rpc_us_p99 | decr_per_s | retrans_pct |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| t100-r1 | 1 | none | 100 | 0.737 | 7.37 | 97 | 187 | 60.4 | 0 | 4.77 | 157 | 8.43e+03 | 0.00219 |
| t100-r2 | 2 | none | 100 | 0.876 | 8.76 | 99 | 193 | 62.4 | 0 | 0 | 164 | 8.51e+03 | 0.00464 |
| t100-r3 | 3 | none | 100 | 1.61 | 16.1 | 96 | 181 | 59.4 | 0 | 30.3 | 181 | 1.12e+04 | 0.000883 |
| t100-r4 | 4 | none | 100 | 0.703 | 7.03 | 98 | 201 | 61.4 | 0 | 0 | 189 | 9.05e+03 | 0.00888 |
| t100-r5 | 5 | none | 100 | 0.788 | 7.88 | 98 | 215 | 61.4 | 0 | 0 | 189 | 1.14e+04 | 0.00434 |
| t150-r4 | 4 | none | 150 | 5.08 | 50.8 | 125 | 243 | 88.4 | 0 | 110 | 266 | 1.02e+04 | 0.00277 |
| t150-r3 | 3 | none | 150 | 2.99 | 29.9 | 123 | 236 | 86.4 | 0 | 93.8 | 224 | 5.71e+03 | 0.00279 |
| t150-r5 | 5 | none | 150 | 9.33 | 93.3 | 135 | 224 | 98.4 | 35.1 | 116 | 275 | 2.44e+04 | 1.61e-05 |
| t150-r1 | 1 | none | 150 | 8.12 | 81.2 | 133 | 227 | 96.4 | 9.69 | 113 | 287 | 1.98e+04 | 0.000345 |
| t150-r2 | 2 | none | 150 | 9.17 | 91.7 | 135 | 224 | 98.4 | 35.1 | 114 | 226 | 2.25e+04 | 0 |
| t200-r1 | 1 | none | 200 | 9.39 | 93.9 | 184 | 262 | 147 | 114 | 170 | 296 | 1.91e+04 | 0 |
| t200-r2 | 2 | none | 200 | 9.54 | 95.4 | 188 | 255 | 151 | 127 | 167 | 282 | 2.08e+04 | 0 |
| t200-r3 | 3 | none | 200 | 9.45 | 94.5 | 188 | 254 | 151 | 128 | 169 | 298 | 2.08e+04 | 0 |
| t200-r4 | 4 | none | 200 | 7.33 | 73.3 | 177 | 272 | 140 | 81.2 | 181 | 303 | 1.39e+04 | 0.000564 |
| t200-r5 | 5 | none | 200 | 4.57 | 45.7 | 168 | 296 | 131 | 0 | 169 | 384 | 7.96e+03 | 0.00289 |
| t300-r4 | 4 | none | 300 | 9.55 | 95.5 | 286 | 356 | 249 | 226 | 270 | 396 | 1.38e+04 | 0 |
| t300-r1 | 1 | none | 300 | 9.54 | 95.4 | 286 | 356 | 249 | 225 | 267 | 409 | 1.37e+04 | 0 |
| t300-r2 | 2 | none | 300 | 9.53 | 95.3 | 283 | 360 | 246 | 223 | 269 | 408 | 1.32e+04 | 8.03e-06 |
| t300-r3 | 3 | none | 300 | 9.52 | 95.2 | 284 | 358 | 247 | 226 | 277 | 401 | 1.32e+04 | 0 |
| t300-r5 | 5 | none | 300 | 9.56 | 95.6 | 286 | 356 | 249 | 225 | 270 | 401 | 1.38e+04 | 0 |

</details>

## E3: incast fan-in sweep

Median (min-max) over n repeats.

| rto_min | fanin | variant | n | qct_us_p50 | qct_us_p99 | retrans_pct | timeouts | drops | frac_ack_pct |
|---|---|---|---|---|---|---|---|---|---|
| default | 64 | cubic | 3 | 2.15e+05 (2.15e+05-2.15e+05) | 2.22e+05 (2.22e+05-2.22e+05) | 3.68 (3.6-3.71) | 500 (496-508) | 1.16e+04 (1.12e+04-1.16e+04) |  |
| default | 64 | dctcp | 3 | 2.14e+05 (2.14e+05-2.14e+05) | 2.22e+05 (2.22e+05-2.22e+05) | 4.36 (4.19-4.37) | 440 (435-465) | 1.24e+04 (1.19e+04-1.25e+04) |  |
| default | 64 | swift | 3 | 2.06e+05 (2.06e+05-2.06e+05) | 2.09e+05 (2.08e+05-2.13e+05) | 3.04 (2.98-3.16) | 0 (0-0) | 5.99e+03 (5.87e+03-6.08e+03) | 16.4 (16.4-16.9) |
| default | 64 | swift-nofrac | 3 | 2.23e+04 (2.22e+04-2.24e+04) | 2.35e+04 (2.34e+04-2.02e+05) | 0.0648 (0.0531-0.0673) | 0 (0-0) | 65 (61-74) | 0 (0-0) |
| default | 64 | swift-pcap | 3 | 2.06e+05 (2.06e+05-2.06e+05) | 2.08e+05 (2.08e+05-2.09e+05) | 1.95 (1.88-1.96) | 1 (0-1) | 4.66e+03 (4.6e+03-4.82e+03) | 41 (40.8-41.7) |
| default | 128 | cubic | 3 | 2.3e+05 (2.29e+05-2.3e+05) | 6.3e+05 (6.3e+05-6.3e+05) | 7.82 (7.73-7.95) | 6.47e+03 (6.3e+03-6.54e+03) | 5.27e+04 (5.2e+04-5.34e+04) |  |
| default | 128 | dctcp | 3 | 2.28e+05 (2.28e+05-2.29e+05) | 6.3e+05 (6.3e+05-6.3e+05) | 8.62 (8.59-8.76) | 6.41e+03 (6.33e+03-6.45e+03) | 5.34e+04 (5.3e+04-5.4e+04) |  |
| default | 128 | swift | 3 | 2.17e+05 (2.16e+05-2.18e+05) | 2.42e+05 (2.4e+05-2.48e+05) | 5.54 (5.54-5.67) | 212 (207-220) | 2.32e+04 (2.3e+04-2.36e+04) | 21.7 (21.2-22) |
| default | 128 | swift-nofrac | 3 | 2.22e+05 (2.22e+05-2.22e+05) | 2.27e+05 (2.27e+05-2.29e+05) | 1.22 (1.19-1.24) | 18 (15-21) | 1.07e+04 (1.05e+04-1.08e+04) | 0 (0-0) |
| default | 128 | swift-pcap | 3 | 2.15e+05 (2.15e+05-2.15e+05) | 2.22e+05 (2.21e+05-2.22e+05) | 3.42 (3.36-3.43) | 148 (137-151) | 1.93e+04 (1.9e+04-1.95e+04) | 59.1 (58.7-59.8) |

<details><summary>Every run</summary>

| run | rep | variant | rto_min | fanin | qct_us_p50 | qct_us_p99 | fct_us_p99 | retrans_pct | timeouts | drops | queue_us_p99 | frac_ack_pct |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cubic-n64-r1 | 1 | cubic | default | 64 | 2.15e+05 | 2.22e+05 | 2.15e+05 | 3.68 | 500 | 1.16e+04 | 116 |  |
| cubic-n64-r2 | 2 | cubic | default | 64 | 2.15e+05 | 2.22e+05 | 2.16e+05 | 3.71 | 508 | 1.16e+04 | 116 |  |
| cubic-n64-r3 | 3 | cubic | default | 64 | 2.15e+05 | 2.22e+05 | 2.21e+05 | 3.6 | 496 | 1.12e+04 | 118 |  |
| dctcp-n64-r1 | 1 | dctcp | default | 64 | 2.14e+05 | 2.22e+05 | 2.15e+05 | 4.19 | 440 | 1.19e+04 | 120 |  |
| dctcp-n64-r2 | 2 | dctcp | default | 64 | 2.14e+05 | 2.22e+05 | 2.15e+05 | 4.36 | 465 | 1.24e+04 | 120 |  |
| dctcp-n64-r3 | 3 | dctcp | default | 64 | 2.14e+05 | 2.22e+05 | 2.14e+05 | 4.37 | 435 | 1.25e+04 | 117 |  |
| swift-n64-r1 | 1 | swift | default | 64 | 2.06e+05 | 2.08e+05 | 2.06e+05 | 3.04 | 0 | 5.99e+03 | 8.1 | 16.9 |
| swift-n64-r2 | 2 | swift | default | 64 | 2.06e+05 | 2.13e+05 | 2.06e+05 | 3.16 | 0 | 6.08e+03 | 3.1 | 16.4 |
| swift-n64-r3 | 3 | swift | default | 64 | 2.06e+05 | 2.09e+05 | 2.07e+05 | 2.98 | 0 | 5.87e+03 | 18.5 | 16.4 |
| swift-nofrac-n64-r1 | 1 | swift-nofrac | default | 64 | 2.24e+04 | 2.02e+05 | 2.34e+04 | 0.0531 | 0 | 65 | 70.3 | 0 |
| swift-nofrac-n64-r2 | 2 | swift-nofrac | default | 64 | 2.23e+04 | 2.35e+04 | 2.35e+04 | 0.0648 | 0 | 61 | 68.4 | 0 |
| swift-nofrac-n64-r3 | 3 | swift-nofrac | default | 64 | 2.22e+04 | 2.34e+04 | 2.33e+04 | 0.0673 | 0 | 74 | 74.1 | 0 |
| swift-pcap-n64-r1 | 1 | swift-pcap | default | 64 | 2.06e+05 | 2.08e+05 | 2.07e+05 | 1.88 | 1 | 4.6e+03 | 19.6 | 40.8 |
| swift-pcap-n64-r2 | 2 | swift-pcap | default | 64 | 2.06e+05 | 2.09e+05 | 2.07e+05 | 1.96 | 0 | 4.66e+03 | 23.3 | 41.7 |
| swift-pcap-n64-r3 | 3 | swift-pcap | default | 64 | 2.06e+05 | 2.08e+05 | 2.07e+05 | 1.95 | 1 | 4.82e+03 | 30.3 | 41 |
| cubic-n128-r1 | 1 | cubic | default | 128 | 2.3e+05 | 6.3e+05 | 2.31e+05 | 7.73 | 6.54e+03 | 5.2e+04 | 120 |  |
| cubic-n128-r2 | 2 | cubic | default | 128 | 2.29e+05 | 6.3e+05 | 2.31e+05 | 7.95 | 6.3e+03 | 5.34e+04 | 119 |  |
| cubic-n128-r3 | 3 | cubic | default | 128 | 2.3e+05 | 6.3e+05 | 2.31e+05 | 7.82 | 6.47e+03 | 5.27e+04 | 119 |  |
| dctcp-n128-r1 | 1 | dctcp | default | 128 | 2.29e+05 | 6.3e+05 | 2.3e+05 | 8.76 | 6.41e+03 | 5.4e+04 | 120 |  |
| dctcp-n128-r2 | 2 | dctcp | default | 128 | 2.28e+05 | 6.3e+05 | 2.3e+05 | 8.62 | 6.33e+03 | 5.3e+04 | 122 |  |
| dctcp-n128-r3 | 3 | dctcp | default | 128 | 2.28e+05 | 6.3e+05 | 2.3e+05 | 8.59 | 6.45e+03 | 5.34e+04 | 121 |  |
| swift-n128-r1 | 1 | swift | default | 128 | 2.17e+05 | 2.48e+05 | 2.18e+05 | 5.54 | 207 | 2.3e+04 | 97.3 | 21.7 |
| swift-n128-r2 | 2 | swift | default | 128 | 2.16e+05 | 2.42e+05 | 2.16e+05 | 5.67 | 220 | 2.36e+04 | 90.8 | 22 |
| swift-n128-r3 | 3 | swift | default | 128 | 2.18e+05 | 2.4e+05 | 2.17e+05 | 5.54 | 212 | 2.32e+04 | 67.7 | 21.2 |
| swift-nofrac-n128-r1 | 1 | swift-nofrac | default | 128 | 2.22e+05 | 2.27e+05 | 2.27e+05 | 1.22 | 18 | 1.07e+04 | 83.8 | 0 |
| swift-nofrac-n128-r2 | 2 | swift-nofrac | default | 128 | 2.22e+05 | 2.29e+05 | 2.29e+05 | 1.19 | 15 | 1.05e+04 | 82.4 | 0 |
| swift-nofrac-n128-r3 | 3 | swift-nofrac | default | 128 | 2.22e+05 | 2.27e+05 | 2.26e+05 | 1.24 | 21 | 1.08e+04 | 87.2 | 0 |
| swift-pcap-n128-r1 | 1 | swift-pcap | default | 128 | 2.15e+05 | 2.22e+05 | 2.17e+05 | 3.42 | 137 | 1.93e+04 | 75.6 | 59.8 |
| swift-pcap-n128-r2 | 2 | swift-pcap | default | 128 | 2.15e+05 | 2.21e+05 | 2.17e+05 | 3.36 | 151 | 1.9e+04 | 78.6 | 58.7 |
| swift-pcap-n128-r3 | 3 | swift-pcap | default | 128 | 2.15e+05 | 2.22e+05 | 2.18e+05 | 3.43 | 148 | 1.95e+04 | 78.1 | 59.1 |

</details>
