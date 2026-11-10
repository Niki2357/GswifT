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

// Flow-based scaling: extra target proportional to 1/sqrt(cwnd), from
// fs_range at fs_min_cwnd down to 0 at fs_max_cwnd.
static inline sw_u32 sw_flow_scale_us(const struct swift_params *p,
                                      sw_u64 cwnd_fp)
{
#ifdef SW_NO_FLOW_SCALING
    // BPF build: the sqrt below currently exceeds the verifier's budget.
    (void)p;
    (void)cwnd_fp;
    return 0;
#else
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
#endif
}

// Fills derived fields. Call once after setting parameters (userspace).
static inline void sw_params_finalize(struct swift_params *p)
{
    p->fs_inv_lo_fp = sw_inv_sqrt_fp(p->fs_max_cwnd_fp);
    p->fs_inv_hi_fp = sw_inv_sqrt_fp(p->fs_min_cwnd_fp);
}

static inline sw_u32 sw_target_us(const struct swift_params *p, sw_u64 cwnd_fp,
                                  sw_u32 hops)
{
    return p->base_target_us + hops * p->hop_scale_us +
           sw_flow_scale_us(p, cwnd_fp);
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
    sw_u32 target = sw_target_us(p, s->cwnd_fp, s->hops);
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

// Integer window handed to Linux, which cannot go below 1.
static inline sw_u32 sw_snd_cwnd(sw_u64 cwnd_fp)
{
    sw_u64 w = cwnd_fp >> SW_FP_SHIFT;
    if (w < 1)
        return 1;
    return w > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (sw_u32)w;
}

// RTT used to turn cwnd into a pacing rate. srtt includes queueing, so during
// an incast it can be many times the base RTT and stretch sub-packet gaps far
// beyond what the window intends. With pace_rtt_min, sub-packet windows pace
// from the minimum RTT instead.
static inline sw_u64 sw_pace_rtt_us(const struct swift_params *p, sw_u64 cwnd_fp,
                                    sw_u64 srtt_us, sw_u64 min_rtt_us)
{
    if (p->pace_rtt_min && cwnd_fp < SW_FP_ONE && min_rtt_us)
        return min_rtt_us;
    return srtt_us;
}

// Pacing rate in bytes/s. 0 means "do not pace".
// Below one packet: one MSS every srtt/cwnd, as in Swift.
// At or above one packet: gain x cwnd x mss / srtt, or unpaced if gain is 0.
static inline sw_u64 sw_pacing_Bps(const struct swift_params *p, sw_u64 cwnd_fp,
                                   sw_u32 mss, sw_u64 srtt_us)
{
    if (srtt_us == 0)
        return 0;
    sw_u64 bytes_per_rtt = (cwnd_fp * mss) >> SW_FP_SHIFT;
    if (bytes_per_rtt == 0)
        bytes_per_rtt = 1;
    sw_u64 rate = bytes_per_rtt * SW_US_PER_S / srtt_us;
    if (cwnd_fp < SW_FP_ONE) {
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
