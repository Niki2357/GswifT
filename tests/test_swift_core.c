// Unit tests for src/swift_core.h, the pure Swift algorithm shared with BPF.
// Build and run:  make -C tests

#include <stdio.h>
#include <stdlib.h>

#include "../src/swift_core.h"

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define FP(x) ((sw_u64)((x) * (double)SW_FP_ONE))
#define NEAR(a, b, tol) ((a) > (b) ? (a) - (b) <= (tol) : (b) - (a) <= (tol))

static struct swift_params defaults(void)
{
    struct swift_params p = {
        .base_target_us = 100,
        .hop_scale_us = 0,
        .default_hops = 1,
        .fs_range_us = 0,
        .fs_min_cwnd_fp = FP(0.1),
        .fs_max_cwnd_fp = FP(100),
        .ai_fp = FP(1.0),
        .beta_fp = FP(0.8),
        .max_mdf_fp = FP(0.5),
        .min_cwnd_fp = FP(0.01),
        .max_cwnd_fp = FP(10000),
        .init_cwnd_fp = FP(10),
        .retx_reset = 3,
        .median_cap = 0,
        .reorder_k = 1,
        .pace_gain_pct = 0,
    };
    return p;
}

static struct swift_state fresh(const struct swift_params *p, sw_u64 cwnd_fp)
{
    struct swift_state s;
    sw_init(p, &s, 1);
    s.cwnd_fp = cwnd_fp;
    return s;
}

static void test_isqrt(void)
{
    CHECK(sw_isqrt(0) == 0);
    CHECK(sw_isqrt(1) == 1);
    CHECK(sw_isqrt(15) == 3);
    CHECK(sw_isqrt(16) == 4);
    CHECK(sw_isqrt(1000000) == 1000);
    CHECK(sw_isqrt(0xFFFFFFFFFFFFFFFFULL) == 0xFFFFFFFFULL);
}

static void test_target_base_and_hops(void)
{
    struct swift_params p = defaults();
    CHECK(sw_target_us(&p, FP(10), 1) == 100);
    p.hop_scale_us = 5;
    CHECK(sw_target_us(&p, FP(10), 4) == 120);
}

static void test_target_flow_scaling(void)
{
    struct swift_params p = defaults();
    p.fs_range_us = 200;
    // At or below fs_min: full range. At or above fs_max: none.
    CHECK(sw_target_us(&p, FP(0.05), 1) == 300);
    CHECK(sw_target_us(&p, FP(0.1), 1) == 300);
    CHECK(sw_target_us(&p, FP(100), 1) == 100);
    CHECK(sw_target_us(&p, FP(500), 1) == 100);
    // Monotonic decreasing in cwnd, strictly inside the range.
    sw_u32 t1 = sw_target_us(&p, FP(1), 1);
    sw_u32 t10 = sw_target_us(&p, FP(10), 1);
    CHECK(t1 > t10);
    CHECK(t1 < 300 && t10 > 100);
    // 1/sqrt shape: (1/sqrt(1) - 1/sqrt(100)) / (1/sqrt(.1) - 1/sqrt(100))
    //             = 0.9 / 3.0623 = 0.2939 -> 100 + 58.8
    CHECK(NEAR(t1, 159, 2));
}

static void test_additive_increase(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(10));
    CHECK(sw_on_ack(&p, &s, 1000, 50, 50, 1) == SW_INCREASE);
    CHECK(NEAR(s.cwnd_fp, FP(10.1), 2));

    // Below one packet the increase is ai per ACK, not ai/cwnd.
    s = fresh(&p, FP(0.5));
    sw_on_ack(&p, &s, 1000, 50, 50, 1);
    CHECK(NEAR(s.cwnd_fp, FP(1.5), 2));

    // Increase scales with packets acked.
    s = fresh(&p, FP(10));
    sw_on_ack(&p, &s, 1000, 50, 50, 5);
    CHECK(NEAR(s.cwnd_fp, FP(10.5), 2));
}

static void test_multiplicative_decrease(void)
{
    struct swift_params p = defaults();
    // delay = 2 x target: factor = 1 - 0.8 * 0.5 = 0.6
    struct swift_state s = fresh(&p, FP(10));
    CHECK(sw_on_ack(&p, &s, 1000, 200, 200, 1) == SW_DECREASE);
    CHECK(NEAR(s.cwnd_fp, FP(6.0), 4));
    CHECK(s.t_last_decrease_us == 1000);

    // delay = 10 x target: 1 - 0.8 * 0.9 = 0.28, capped at 1 - max_mdf = 0.5
    s = fresh(&p, FP(10));
    sw_on_ack(&p, &s, 1000, 1000, 1000, 1);
    CHECK(NEAR(s.cwnd_fp, FP(5.0), 4));

    // Delay exactly at target is not below target: decrease by 0.
    s = fresh(&p, FP(10));
    sw_on_ack(&p, &s, 1000, 100, 100, 1);
    CHECK(s.cwnd_fp == FP(10));
}

static void test_decrease_once_per_rtt(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(10));
    sw_on_ack(&p, &s, 10000, 200, 200, 1);
    sw_u64 after_first = s.cwnd_fp;
    // Within one srtt of the last decrease: hold.
    CHECK(sw_on_ack(&p, &s, 10100, 200, 200, 1) == SW_HOLD);
    CHECK(s.cwnd_fp == after_first);
    // After one srtt: decrease again.
    CHECK(sw_on_ack(&p, &s, 10200, 200, 200, 1) == SW_DECREASE);
    CHECK(s.cwnd_fp < after_first);
}

static void test_clamps(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(0.015));
    sw_on_ack(&p, &s, 1000, 1000, 1000, 1);
    CHECK(s.cwnd_fp == p.min_cwnd_fp);

    s = fresh(&p, FP(10000));
    sw_on_ack(&p, &s, 1000, 10, 10, 100);
    CHECK(s.cwnd_fp == p.max_cwnd_fp);
}

static void test_invalid_sample_ignored(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(10));
    CHECK(sw_on_ack(&p, &s, 1000, 0, 50, 1) == SW_NONE);
    CHECK(sw_on_ack(&p, &s, 1000, -1, 50, 1) == SW_NONE);
    CHECK(s.cwnd_fp == FP(10));
}

static void test_rto(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(10));
    sw_on_rto(&p, &s, 1000, 100);
    CHECK(s.retx_cnt == 1);
    CHECK(NEAR(s.cwnd_fp, FP(5), 2));
    sw_on_rto(&p, &s, 1050, 100);  // within an RTT: counted, not decreased
    CHECK(s.retx_cnt == 2);
    CHECK(NEAR(s.cwnd_fp, FP(5), 2));
    sw_on_rto(&p, &s, 5000, 100);  // third in a row: reset to min
    CHECK(s.retx_cnt == 3);
    CHECK(s.cwnd_fp == p.min_cwnd_fp);
    // A valid ACK clears the retransmit count.
    sw_on_ack(&p, &s, 6000, 50, 50, 1);
    CHECK(s.retx_cnt == 0);
}

static void test_recovery(void)
{
    struct swift_params p = defaults();
    struct swift_state s = fresh(&p, FP(10));
    s.retx_cnt = 2;
    sw_on_recovery(&p, &s, 1000, 100);
    CHECK(s.retx_cnt == 0);
    CHECK(NEAR(s.cwnd_fp, FP(5), 2));
    sw_on_recovery(&p, &s, 1050, 100);
    CHECK(NEAR(s.cwnd_fp, FP(5), 2));
}

static void test_snd_cwnd(void)
{
    CHECK(sw_snd_cwnd(FP(0.01)) == 1);
    CHECK(sw_snd_cwnd(FP(0.99)) == 1);
    CHECK(sw_snd_cwnd(FP(1.0)) == 1);
    CHECK(sw_snd_cwnd(FP(7.9)) == 7);
}

static void test_pacing(void)
{
    struct swift_params p = defaults();
    // cwnd 0.5, mss 1000 B, srtt 100 us: one packet per 200 us = 5 MB/s.
    CHECK(NEAR(sw_pacing_Bps(&p, FP(0.5), 1000, 100), 5000000ULL, 100));
    // cwnd >= 1 and gain 0: unpaced.
    CHECK(sw_pacing_Bps(&p, FP(4), 1000, 100) == 0);
    // gain 200%: 2 x 4 x 1000 / 100us = 80 MB/s.
    p.pace_gain_pct = 200;
    CHECK(NEAR(sw_pacing_Bps(&p, FP(4), 1000, 100), 80000000ULL, 100));
    // No RTT estimate yet: unpaced rather than divide by zero.
    CHECK(sw_pacing_Bps(&p, FP(0.5), 1000, 0) == 0);
}

static void test_median_window_size(void)
{
    struct swift_params p = defaults();
    p.median_cap = 16;
    CHECK(sw_median_h(&p, FP(0.5)) == 1);
    CHECK(sw_median_h(&p, FP(4)) == 2);
    CHECK(sw_median_h(&p, FP(9)) == 4);
    CHECK(sw_median_h(&p, FP(100)) == 16);
    p.median_cap = 0;
    CHECK(sw_median_h(&p, FP(100)) == 1);
}

// Feed `n` samples where the first `slow` of every 16 take the congested path.
static int count_decreases(const struct swift_params *p, struct swift_state *s,
                           int n, int slow)
{
    int decreases = 0;
    for (int i = 0; i < n; i++) {
        long rtt = (i % 16) < slow ? 400 : 50;
        if (sw_on_ack(p, s, 100000 + i * 1000, rtt, 60, 1) == SW_DECREASE)
            decreases++;
    }
    return decreases;
}

static void test_median_filters_minority(void)
{
    struct swift_params p = defaults();
    p.median_cap = 16;
    struct swift_state s = fresh(&p, FP(40));  // H = 16
    count_decreases(&p, &s, 16, 0);            // warm the sample ring
    // 3 of every 16 samples go over a congested path (q ~ 0.19).
    CHECK(count_decreases(&p, &s, 64, 3) == 0);

    // Baseline Swift on the same samples decreases.
    p.median_cap = 0;
    s = fresh(&p, FP(40));
    count_decreases(&p, &s, 16, 0);
    CHECK(count_decreases(&p, &s, 64, 3) > 0);
}

static void test_median_reacts_to_majority(void)
{
    struct swift_params p = defaults();
    p.median_cap = 16;
    struct swift_state s = fresh(&p, FP(40));
    int decreases = 0;
    for (int i = 0; i < 32; i++)
        if (sw_on_ack(&p, &s, 1000 + i * 1000, 400, 60, 1) == SW_DECREASE)
            decreases++;
    CHECK(decreases > 0);
}

static void test_reorder_k(void)
{
    struct swift_params p = defaults();
    p.reorder_k = 3;
    struct swift_state s = fresh(&p, FP(10));
    CHECK(sw_on_ack(&p, &s, 1000, 400, 60, 1) == SW_HOLD);
    CHECK(sw_on_ack(&p, &s, 2000, 400, 60, 1) == SW_HOLD);
    CHECK(sw_on_ack(&p, &s, 3000, 50, 60, 1) == SW_INCREASE);  // run broken
    CHECK(sw_on_ack(&p, &s, 4000, 400, 60, 1) == SW_HOLD);
    CHECK(sw_on_ack(&p, &s, 5000, 400, 60, 1) == SW_HOLD);
    CHECK(sw_on_ack(&p, &s, 6000, 400, 60, 1) == SW_DECREASE);
}

static void test_ring_saturates(void)
{
    struct swift_params p = defaults();
    p.median_cap = 16;
    struct swift_state s = fresh(&p, FP(1));
    sw_on_ack(&p, &s, 1000, 100000, 60, 1);
    CHECK(s.ring[0] == 0xFFFF);
}

static void test_state_fits_ca_priv(void)
{
    CHECK(sizeof(struct swift_state) <= 104);
}

int main(void)
{
    test_isqrt();
    test_target_base_and_hops();
    test_target_flow_scaling();
    test_additive_increase();
    test_multiplicative_decrease();
    test_decrease_once_per_rtt();
    test_clamps();
    test_invalid_sample_ignored();
    test_rto();
    test_recovery();
    test_snd_cwnd();
    test_pacing();
    test_median_window_size();
    test_median_filters_minority();
    test_median_reacts_to_majority();
    test_reorder_k();
    test_ring_saturates();
    test_state_fits_ca_priv();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
