// Swift congestion control (Kumar et al., SIGCOMM 2020, Algorithm 1), plus the
// MSwift median filter (Gerstein et al., 2025), as pure functions over a state
// struct.
//
// This header is compiled into the BPF module and into the userspace unit
// tests, so it follows BPF rules: no floats, no libc, no recursion, and only
// loops with constant bounds. All fractional values are fixed point x 2^16.

#ifndef SWIFT_CORE_H
#define SWIFT_CORE_H

#include "swift_types.h"

#define SW_US_PER_S 1000000ULL

// 1 if a < b, else 0, without a branch. Requires a, b < 2^63.
// (Branches inside loops multiply the paths the BPF verifier must explore.)
static inline sw_u64 sw_lt(sw_u64 a, sw_u64 b)
{
    return (a - b) >> 63;
}

#define SW_ISQRT_MAX ((1ULL << 62) - 1)

// Integer square root, floor(sqrt(x)), for x <= 2^62 - 1 (larger inputs are
// clamped). Branch-free body, constant 32 iterations.
static inline sw_u64 sw_isqrt(sw_u64 x)
{
    sw_u64 res = 0, bit = 1ULL << 62;
    if (x > SW_ISQRT_MAX)
        x = SW_ISQRT_MAX;
#ifdef __clang__
#pragma unroll
#endif
    for (int i = 0; i < 32; i++) {
        sw_u64 t = res + bit;
        sw_u64 mask = 0 - (1 - sw_lt(x, t));  // all ones if x >= t
        x -= t & mask;
        res = (res >> 1) + (bit & mask);
        bit >>= 2;
    }
    return res;
}

// 1/sqrt(cwnd) in fixed point. Large value for cwnd near 0.
static inline sw_u64 sw_inv_sqrt_fp(sw_u64 cwnd_fp)
{
    sw_u64 r = sw_isqrt(cwnd_fp << SW_FP_SHIFT);  // sqrt(cwnd) x 2^16
    if (r == 0)
        return 1ULL << 32;
    return (1ULL << 32) / r;
}

static inline sw_u64 sw_clamp(sw_u64 v, sw_u64 lo, sw_u64 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Flow-scaling table index for a window, or -1 at 64 packets and above.
static inline int sw_fs_index(sw_u64 cwnd_fp)
{
    if (cwnd_fp < (1ULL << SW_FP_SHIFT))
        return (int)(cwnd_fp >> (SW_FP_SHIFT - 8));
    if (cwnd_fp < (8ULL << SW_FP_SHIFT))
        return SW_FS_T0 + (int)((cwnd_fp - (1ULL << SW_FP_SHIFT)) >> (SW_FP_SHIFT - 5));
    if (cwnd_fp < (64ULL << SW_FP_SHIFT))
        return SW_FS_T0 + SW_FS_T1 +
               (int)((cwnd_fp - (8ULL << SW_FP_SHIFT)) >> (SW_FP_SHIFT - 1));
    return -1;
}

// Window at the middle of table bucket i (Q16), for filling the table.
static inline sw_u64 sw_fs_bucket_mid(int i)
{
    if (i < SW_FS_T0)
        return ((sw_u64)i << (SW_FP_SHIFT - 8)) + (1ULL << (SW_FP_SHIFT - 9));
    i -= SW_FS_T0;
    if (i < SW_FS_T1)
        return (1ULL << SW_FP_SHIFT) + ((sw_u64)i << (SW_FP_SHIFT - 5)) +
               (1ULL << (SW_FP_SHIFT - 6));
    i -= SW_FS_T1;
    return (8ULL << SW_FP_SHIFT) + ((sw_u64)i << (SW_FP_SHIFT - 1)) +
           (1ULL << (SW_FP_SHIFT - 2));
}

// Flow scaling by table lookup: what the BPF build uses. Zero at 64 packets
// and above (the exact value there is under 1% of the range with the
// default fs_max_cwnd of 100).
static inline sw_u32 sw_flow_scale_table_us(const struct swift_params *p,
                                            sw_u64 cwnd_fp)
{
    if (p->fs_range_us == 0)
        return 0;
    if (cwnd_fp <= p->fs_min_cwnd_fp)
        return p->fs_range_us;
    int i = sw_fs_index(cwnd_fp);
    if (i < 0 || i >= SW_FS_TABLE)
        return 0;
    return p->fs_table[i];
}

#ifndef SW_NO_FLOW_SCALING
// Flow-based scaling: extra target proportional to 1/sqrt(cwnd), from
// fs_range at fs_min_cwnd down to 0 at fs_max_cwnd. Exact; userspace only.
static inline sw_u32 sw_flow_scale_us(const struct swift_params *p,
                                      sw_u64 cwnd_fp)
{
    if (p->fs_range_us == 0)
        return 0;
    if (cwnd_fp <= p->fs_min_cwnd_fp)
        return p->fs_range_us;
    if (cwnd_fp >= p->fs_max_cwnd_fp)
        return 0;
    sw_u64 inv_c = sw_inv_sqrt_fp(cwnd_fp);
    sw_u64 inv_lo = p->fs_inv_lo_fp;
    sw_u64 inv_hi = p->fs_inv_hi_fp;
    if (inv_hi <= inv_lo || inv_c <= inv_lo)
        return 0;
    return (sw_u32)((sw_u64)p->fs_range_us * (inv_c - inv_lo) /
                    (inv_hi - inv_lo));
}
#else
// BPF build: the per-ACK sqrt exceeds the verifier's budget; use the table
// the loader filled (sw_params_finalize).
static inline sw_u32 sw_flow_scale_us(const struct swift_params *p,
                                      sw_u64 cwnd_fp)
{
    return sw_flow_scale_table_us(p, cwnd_fp);
}
#endif

// Fills derived fields. Call once after setting parameters (userspace).
static inline void sw_params_finalize(struct swift_params *p)
{
    p->fs_inv_lo_fp = sw_inv_sqrt_fp(p->fs_max_cwnd_fp);
    p->fs_inv_hi_fp = sw_inv_sqrt_fp(p->fs_min_cwnd_fp);
#ifndef SW_NO_FLOW_SCALING
    for (int i = 0; i < SW_FS_TABLE; i++) {
        sw_u32 v = sw_flow_scale_us(p, sw_fs_bucket_mid(i));
        p->fs_table[i] = v > 0xFFFF ? 0xFFFF : (sw_u16)v;
    }
#endif
}

static inline sw_u32 sw_target_us(const struct swift_params *p, sw_u64 cwnd_fp,
                                  sw_u32 hops)
{
    return p->base_target_us + hops * p->hop_scale_us +
           sw_flow_scale_us(p, cwnd_fp);
}

// Target for this flow: sw_target_us plus, with target_floor, the flow's
// measured RTT floor. base_target is then headroom above the floor rather
// than an absolute delay, so one setting carries across hosts.
static inline sw_u32 sw_flow_target_us(const struct swift_params *p,
                                       const struct swift_state *s)
{
    sw_u32 t = sw_target_us(p, s->cwnd_fp, s->hops) + s->target_off_us;
    return p->target_floor ? t + s->floor_us : t;
}

// Per-flow target offset from a random 32-bit value: uniform in [0, J).
static inline sw_u16 sw_target_offset(const struct swift_params *p, sw_u32 rnd)
{
    sw_u32 j = p->target_jitter_us > 0xFFFF ? 0xFFFF : p->target_jitter_us;
    return j ? (sw_u16)(rnd % j) : 0;
}

static inline void sw_init(const struct swift_params *p, struct swift_state *s,
                           sw_u32 hops)
{
    __builtin_memset(s, 0, sizeof(*s));
    s->cwnd_fp = sw_clamp(p->init_cwnd_fp, p->min_cwnd_fp, p->max_cwnd_fp);
    s->hops = hops > 255 ? 255 : (sw_u8)hops;
}

// MSwift median window: H = max(cwnd/2, 1), capped. 1 means no filtering.
static inline sw_u32 sw_median_h(const struct swift_params *p, sw_u64 cwnd_fp)
{
    if (p->median_cap <= 1)
        return 1;
    sw_u64 h = cwnd_fp >> (SW_FP_SHIFT + 1);
    if (h < 1)
        return 1;
    return h > p->median_cap ? p->median_cap : (sw_u32)h;
}

static inline void sw_ring_push(struct swift_state *s, sw_u64 rtt_us)
{
    s->ring[s->ring_idx & SW_RING_MASK] =
        rtt_us > 0xFFFF ? 0xFFFF : (sw_u16)rtt_us;
    s->ring_idx = (s->ring_idx + 1) & SW_RING_MASK;
    if (s->ring_cnt < SW_RING_SIZE)
        s->ring_cnt++;
}

// Lower median of the newest n samples in the ring (1 <= n <= 16).
static inline sw_u32 sw_ring_median(const struct swift_state *s, sw_u32 n)
{
    sw_u32 k = (n - 1) / 2;  // rank of the lower median
    sw_u32 newest = s->ring_idx + SW_RING_SIZE - 1;
    for (sw_u32 i = 0; i < SW_RING_SIZE; i++) {
        if (i >= n)
            break;
        sw_u32 v = s->ring[(newest - i) & SW_RING_MASK];
        sw_u32 less = 0, leq = 0;
        for (sw_u32 j = 0; j < SW_RING_SIZE; j++) {
            if (j >= n)
                break;
            sw_u32 w = s->ring[(newest - j) & SW_RING_MASK];
            less += (sw_u32)sw_lt(w, v);
            leq += (sw_u32)sw_lt(w, (sw_u64)v + 1);
        }
        if (less <= k && k < leq)
            return v;
    }
    return s->ring[(newest) & SW_RING_MASK];
}

// Delay signal the controller reacts to: the raw sample, or MSwift's median.
static inline sw_u32 sw_delay_signal(const struct swift_params *p,
                                     struct swift_state *s, sw_u64 rtt_us)
{
    if (p->median_cap <= 1)
        return rtt_us > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (sw_u32)rtt_us;
    sw_ring_push(s, rtt_us);
    sw_u32 h = sw_median_h(p, s->cwnd_fp);
    sw_u32 n = h < s->ring_cnt ? h : s->ring_cnt;
    return sw_ring_median(s, n);
}

static inline int sw_can_decrease(const struct swift_state *s, sw_u64 now_us,
                                  sw_u64 srtt_us)
{
    return now_us - s->t_last_decrease_us >= srtt_us;
}

// Multiply cwnd by a factor, record the decrease time, and clamp.
static inline void sw_scale_down(const struct swift_params *p,
                                 struct swift_state *s, sw_u64 factor_fp,
                                 sw_u64 now_us)
{
    s->cwnd_fp = (s->cwnd_fp * factor_fp) >> SW_FP_SHIFT;
    s->cwnd_fp = sw_clamp(s->cwnd_fp, p->min_cwnd_fp, p->max_cwnd_fp);
    s->t_last_decrease_us = now_us;
}

static inline sw_u64 sw_mdf_floor(const struct swift_params *p)
{
    return p->max_mdf_fp >= SW_FP_ONE ? 0 : SW_FP_ONE - p->max_mdf_fp;
}

static inline void sw_increase(const struct swift_params *p,
                               struct swift_state *s, sw_u32 acked)
{
    if (s->cwnd_fp >= SW_FP_ONE)
        s->cwnd_fp += (p->ai_fp * acked << SW_FP_SHIFT) / s->cwnd_fp;
    else
        s->cwnd_fp += p->ai_fp * acked;
    s->cwnd_fp = sw_clamp(s->cwnd_fp, p->min_cwnd_fp, p->max_cwnd_fp);
}

static inline void sw_decrease(const struct swift_params *p,
                               struct swift_state *s, sw_u32 delay,
                               sw_u32 target, sw_u64 now_us)
{
    sw_u64 cut = p->beta_fp * (sw_u64)(delay - target) / delay;
    sw_u64 factor = cut >= SW_FP_ONE ? 0 : SW_FP_ONE - cut;
    sw_u64 floor = sw_mdf_floor(p);
    sw_scale_down(p, s, factor > floor ? factor : floor, now_us);
}

// Per-ACK update. rtt_us <= 0 means the ACK carried no RTT sample.
static inline int sw_on_ack(const struct swift_params *p, struct swift_state *s,
                            sw_u64 now_us, sw_s64 rtt_us, sw_u64 srtt_us,
                            sw_u32 acked)
{
    if (rtt_us <= 0)
        return SW_NONE;
    s->retx_cnt = 0;
    sw_u32 target = sw_flow_target_us(p, s);
    sw_u32 delay = sw_delay_signal(p, s, (sw_u64)rtt_us);
    s->target_us = target;
    s->delay_us = delay;

    if (delay < target) {
        s->consec_over = 0;
        sw_increase(p, s, acked);
        return SW_INCREASE;
    }
    if (s->consec_over < 255)
        s->consec_over++;
    if (s->consec_over < p->reorder_k || !sw_can_decrease(s, now_us, srtt_us))
        return SW_HOLD;
    sw_decrease(p, s, delay, target, now_us);
    return SW_DECREASE;
}

// Retransmission timeout.
static inline void sw_on_rto(const struct swift_params *p,
                             struct swift_state *s, sw_u64 now_us,
                             sw_u64 srtt_us)
{
    if (s->retx_cnt < 0xFFFF)
        s->retx_cnt++;
    if (s->retx_cnt >= p->retx_reset) {
        s->cwnd_fp = p->min_cwnd_fp;
        s->t_last_decrease_us = now_us;
    } else if (sw_can_decrease(s, now_us, srtt_us)) {
        sw_scale_down(p, s, sw_mdf_floor(p), now_us);
    }
}

// Entering fast recovery (loss detected by SACK / dupacks).
static inline void sw_on_recovery(const struct swift_params *p,
                                  struct swift_state *s, sw_u64 now_us,
                                  sw_u64 srtt_us)
{
    s->retx_cnt = 0;
    if (sw_can_decrease(s, now_us, srtt_us))
        sw_scale_down(p, s, sw_mdf_floor(p), now_us);
}

// Linux's window floor in packets: 1, or min_snd_cwnd if set.
static inline sw_u32 sw_min_snd_cwnd(const struct swift_params *p)
{
    return p->min_snd_cwnd > 1 ? p->min_snd_cwnd : 1;
}

// Paced regime: cwnd below Linux's window floor, so the window alone cannot
// enforce it and pacing sets the rate. By default this is cwnd < 1. With
// min_snd_cwnd 2, Linux keeps two packets in flight (sent as one 2-segment
// burst per pacing gap), so a single loss can be detected by SACK instead of
// waiting for the tail loss probe.
static inline int sw_paced(const struct swift_params *p, sw_u64 cwnd_fp)
{
    return cwnd_fp < ((sw_u64)sw_min_snd_cwnd(p) << SW_FP_SHIFT);
}

// Integer window handed to Linux, which cannot go below 1.
static inline sw_u32 sw_snd_cwnd(const struct swift_params *p, sw_u64 cwnd_fp)
{
    sw_u64 w = cwnd_fp >> SW_FP_SHIFT;
    sw_u32 lo = sw_min_snd_cwnd(p);
    if (w < lo)
        return lo;
    return w > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (sw_u32)w;
}

// RTT used to turn cwnd into a pacing rate. srtt includes queueing, so during
// an incast it can be many times the base RTT and stretch sub-packet gaps far
// beyond what the window intends. With pace_rtt_min, sub-packet windows pace
// from the minimum RTT instead.
static inline sw_u64 sw_pace_rtt_us(const struct swift_params *p, sw_u64 cwnd_fp,
                                    sw_u64 srtt_us, sw_u64 min_rtt_us)
{
    if (p->pace_rtt_min && sw_paced(p, cwnd_fp) && min_rtt_us)
        return min_rtt_us;
    return srtt_us;
}

// Pacing rate in bytes/s. 0 means "do not pace".
// Paced regime (below one packet by default): one MSS every srtt/cwnd, as in
// Swift. Otherwise: gain x cwnd x mss / srtt, or unpaced if gain is 0.
static inline sw_u64 sw_pacing_Bps(const struct swift_params *p, sw_u64 cwnd_fp,
                                   sw_u32 mss, sw_u64 srtt_us)
{
    if (srtt_us == 0)
        return 0;
    sw_u64 bytes_per_rtt = (cwnd_fp * mss) >> SW_FP_SHIFT;
    if (bytes_per_rtt == 0)
        bytes_per_rtt = 1;
    sw_u64 rate = bytes_per_rtt * SW_US_PER_S / srtt_us;
    if (sw_paced(p, cwnd_fp)) {
        // Optional floor: at least one MSS every max_pace_gap_us.
        if (p->max_pace_gap_us) {
            sw_u64 floor = (sw_u64)mss * SW_US_PER_S / p->max_pace_gap_us;
            if (rate < floor)
                rate = floor;
        }
        return rate;
    }
    if (p->pace_gain_pct == 0)
        return 0;
    return rate * p->pace_gain_pct / 100;
}

#endif
