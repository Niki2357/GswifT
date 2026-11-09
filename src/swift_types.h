// Shared types for the Swift congestion control module.
//
// Included by the BPF program, the userspace loader, and the unit tests, so it
// must not depend on kernel or libc headers beyond fixed-width integers.

#ifndef SWIFT_TYPES_H
#define SWIFT_TYPES_H

#ifdef __bpf__
typedef __u64 sw_u64;
typedef __u32 sw_u32;
typedef __u16 sw_u16;
typedef __u8 sw_u8;
typedef __s64 sw_s64;
#else
#include <stdint.h>
typedef uint64_t sw_u64;
typedef uint32_t sw_u32;
typedef uint16_t sw_u16;
typedef uint8_t sw_u8;
typedef int64_t sw_s64;
#endif

// Fixed point: cwnd and factors are stored as value * 2^16.
#define SW_FP_SHIFT 16
#define SW_FP_ONE (1ULL << SW_FP_SHIFT)

// MSwift median window is capped so it fits in per-socket CA state.
#define SW_RING_SIZE 16
#define SW_RING_MASK (SW_RING_SIZE - 1)

// Tunables. Set by the loader before the BPF object is loaded.
// All fractional values are fixed point (x 2^16).
struct swift_params {
    sw_u32 base_target_us;  // base target delay
    sw_u32 hop_scale_us;    // target added per hop
    sw_u32 default_hops;    // used when no per-destination entry exists
    sw_u32 fs_range_us;     // max extra target from flow scaling (0 = off)
    sw_u64 fs_min_cwnd_fp;  // cwnd at which flow scaling gives fs_range
    sw_u64 fs_max_cwnd_fp;  // cwnd at which flow scaling gives 0
    sw_u64 ai_fp;           // additive increase, packets per RTT
    sw_u64 beta_fp;         // multiplicative decrease scale
    sw_u64 max_mdf_fp;      // max decrease fraction per decrease
    sw_u64 min_cwnd_fp;     // floor (may be < 1 packet)
    sw_u64 max_cwnd_fp;     // ceiling
    sw_u64 init_cwnd_fp;    // starting window
    sw_u32 retx_reset;      // RTOs in a row before cwnd -> min_cwnd
    sw_u32 median_cap;      // MSwift: max median window H (0 or 1 = off)
    sw_u32 reorder_k;       // MSwift: consecutive over-target samples to decrease
    sw_u32 pace_gain_pct;   // pacing gain when cwnd >= 1 (0 = unpaced)
};

// Runtime configuration, stored in a one-entry BPF array map by the loader.
struct swift_cfg {
    struct swift_params p;
    sw_u32 trace_every;  // emit one ACK record per N ACKs per flow (0 = off)
    sw_u32 trace_events; // also emit decrease / RTO / recovery records
};

// Per-socket state. Must fit in icsk_ca_priv (104 bytes on 5.15 and 6.x).
struct swift_state {
    sw_u64 cwnd_fp;
    sw_u64 t_last_decrease_us;
    sw_u32 target_us;     // last computed target, for telemetry
    sw_u32 delay_us;      // last delay used for the decision
    sw_u32 ack_cnt;       // for trace sampling
    sw_u16 retx_cnt;
    sw_u8 hops;
    sw_u8 ring_idx;
    sw_u8 ring_cnt;
    sw_u8 consec_over;
    sw_u8 pad[2];
    sw_u16 ring[SW_RING_SIZE];  // recent delay samples, us, saturating
};

// Outcome of one ACK, for telemetry.
enum swift_action {
    SW_NONE = 0,
    SW_INCREASE = 1,
    SW_DECREASE = 2,
    SW_HOLD = 3,   // above target but decrease not allowed yet
};

// Telemetry event types.
enum swift_event_type {
    SW_EV_ACK = 0,
    SW_EV_DECREASE = 1,
    SW_EV_RTO = 2,
    SW_EV_RECOVERY = 3,
    SW_EV_INIT = 4,
};

// One telemetry record, written to the ring buffer.
struct swift_event {
    sw_u64 ts_ns;
    sw_u64 cwnd_fp;
    sw_u64 pacing_Bps;
    sw_u32 rtt_us;     // raw sample from this ACK
    sw_u32 srtt_us;
    sw_u32 delay_us;   // signal used (raw or median)
    sw_u32 target_us;
    sw_u32 snd_cwnd;
    sw_u32 inflight;
    sw_u32 acked;
    sw_u16 sport;
    sw_u16 dport;
    sw_u16 retx_cnt;
    sw_u8 type;
    sw_u8 action;
};

#endif
