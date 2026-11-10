// Option parsing and validation for swiftctl.

#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swift_core.h"
#include "swiftctl_args.h"

#define FPV(x) ((sw_u64)((x) * (double)SW_FP_ONE + 0.5))
#define UNFP(x) ((double)(x) / (double)SW_FP_ONE)

enum {
    O_BASE = 1000, O_HOPSCALE, O_HOPS, O_HOP, O_FSRANGE, O_FSMIN, O_FSMAX,
    O_AI, O_BETA, O_MDF, O_MINCWND, O_MAXCWND, O_INITCWND, O_RETX,
    O_MEDIAN, O_REORDER, O_PACE, O_PACE_MINRTT, O_PACE_GAP, O_TRACE_EVERY, O_NO_EVENTS, O_OUT,
    O_STATS, O_DURATION, O_VERBOSE, O_HELP,
};

static const struct option long_opts[] = {
    {"base-target", required_argument, 0, O_BASE},
    {"hop-scale", required_argument, 0, O_HOPSCALE},
    {"hops", required_argument, 0, O_HOPS},
    {"hop", required_argument, 0, O_HOP},
    {"fs-range", required_argument, 0, O_FSRANGE},
    {"fs-min-cwnd", required_argument, 0, O_FSMIN},
    {"fs-max-cwnd", required_argument, 0, O_FSMAX},
    {"ai", required_argument, 0, O_AI},
    {"beta", required_argument, 0, O_BETA},
    {"max-mdf", required_argument, 0, O_MDF},
    {"min-cwnd", required_argument, 0, O_MINCWND},
    {"max-cwnd", required_argument, 0, O_MAXCWND},
    {"init-cwnd", required_argument, 0, O_INITCWND},
    {"retx-reset", required_argument, 0, O_RETX},
    {"median-cap", required_argument, 0, O_MEDIAN},
    {"reorder-k", required_argument, 0, O_REORDER},
    {"pace-gain", required_argument, 0, O_PACE},
    {"pace-min-rtt", no_argument, 0, O_PACE_MINRTT},
    {"max-pace-gap", required_argument, 0, O_PACE_GAP},
    {"trace-every", required_argument, 0, O_TRACE_EVERY},
    {"no-trace-events", no_argument, 0, O_NO_EVENTS},
    {"trace", required_argument, 0, O_OUT},
    {"stats", required_argument, 0, O_STATS},
    {"duration", required_argument, 0, O_DURATION},
    {"verbose", no_argument, 0, O_VERBOSE},
    {"help", no_argument, 0, O_HELP},
    {0, 0, 0, 0},
};

static void usage(FILE *out)
{
    fprintf(out,
        "usage: swiftctl [options]\n"
        "Loads the 'swift' TCP congestion control and runs until SIGINT/SIGTERM\n"
        "(or --duration). Select it with: iperf3 -C swift, or\n"
        "sysctl net.ipv4.tcp_congestion_control=swift\n\n"
        "Target delay (us):\n"
        "  --base-target N     base target delay           [100]\n"
        "  --hop-scale N       extra target per hop        [0]\n"
        "  --hops N            default hop count           [1]\n"
        "  --hop IP=N          hop count for a destination (repeatable)\n"
        "  --fs-range N        flow-scaling range, 0 = off [0]\n"
        "  --fs-min-cwnd X     cwnd for full fs-range      [0.1]\n"
        "  --fs-max-cwnd X     cwnd for zero fs-range      [100]\n"
        "Window (packets, fractional allowed):\n"
        "  --ai X  [1.0]   --beta X  [0.8]   --max-mdf X  [0.5]\n"
        "  --min-cwnd X [0.01]  --max-cwnd X [10000]  --init-cwnd X [10]\n"
        "  --retx-reset N      RTOs before cwnd -> min     [3]\n"
        "  --pace-gain PCT     pacing gain at cwnd >= 1, 0 = unpaced [0]\n"
        "  --pace-min-rtt      below 1 packet, pace from min RTT, not srtt\n"
        "  --max-pace-gap US   below 1 packet, longest gap between sends [0 = none]\n"
        "MSwift:\n"
        "  --median-cap N      median over last min(cwnd/2, N) samples, <=16 [0 = off]\n"
        "  --reorder-k N       decrease after N consecutive over-target [1]\n"
        "Output:\n"
        "  --trace FILE        per-ACK CSV\n"
        "  --trace-every N     one ACK record per N ACKs per flow [0 = off]\n"
        "  --no-trace-events   omit decrease/RTO/recovery records\n"
        "  --stats FILE        counters at exit (key=value)\n"
        "  --duration S        exit after S seconds\n");
}

static void set_defaults(struct swiftctl_opts *o)
{
    memset(o, 0, sizeof(*o));
    struct swift_params *p = &o->cfg.p;
    p->base_target_us = 100;
    p->hop_scale_us = 0;
    p->default_hops = 1;
    p->fs_range_us = 0;
    p->fs_min_cwnd_fp = FPV(0.1);
    p->fs_max_cwnd_fp = FPV(100);
    p->ai_fp = FPV(1.0);
    p->beta_fp = FPV(0.8);
    p->max_mdf_fp = FPV(0.5);
    p->min_cwnd_fp = FPV(0.01);
    p->max_cwnd_fp = FPV(10000);
    p->init_cwnd_fp = FPV(10);
    p->retx_reset = 3;
    p->median_cap = 0;
    p->reorder_k = 1;
    p->pace_gain_pct = 0;
    o->cfg.trace_every = 0;
    o->cfg.trace_events = 1;
}

static int parse_num(const char *s, double lo, double hi, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "swiftctl: '%s' must be a number in [%g, %g]\n", s,
                lo, hi);
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_hop(const char *s, struct swiftctl_opts *o)
{
    char ip[64];
    const char *eq = strchr(s, '=');
    double n;
    if (!eq || (size_t)(eq - s) >= sizeof(ip) || o->n_hops >= SWIFTCTL_MAX_HOPS) {
        fprintf(stderr, "swiftctl: --hop expects IP=N (max %d entries)\n",
                SWIFTCTL_MAX_HOPS);
        return -1;
    }
    memcpy(ip, s, eq - s);
    ip[eq - s] = '\0';
    struct hop_entry *e = &o->hops[o->n_hops];
    if (inet_pton(AF_INET, ip, &e->daddr) != 1) {
        fprintf(stderr, "swiftctl: bad IPv4 address '%s'\n", ip);
        return -1;
    }
    if (parse_num(eq + 1, 0, 255, &n))
        return -1;
    e->hops = (uint32_t)n;
    o->n_hops++;
    return 0;
}

// Cross-field checks after all options are read.
static int validate(const struct swiftctl_opts *o)
{
    const struct swift_params *p = &o->cfg.p;
    if (p->min_cwnd_fp == 0 || p->min_cwnd_fp > p->max_cwnd_fp) {
        fprintf(stderr, "swiftctl: need 0 < min-cwnd <= max-cwnd\n");
        return -1;
    }
    if (p->init_cwnd_fp < p->min_cwnd_fp || p->init_cwnd_fp > p->max_cwnd_fp) {
        fprintf(stderr, "swiftctl: init-cwnd must be within [min, max]\n");
        return -1;
    }
    if (p->fs_range_us && p->fs_min_cwnd_fp >= p->fs_max_cwnd_fp) {
        fprintf(stderr, "swiftctl: need fs-min-cwnd < fs-max-cwnd\n");
        return -1;
    }
    if (p->fs_range_us) {
        fprintf(stderr, "swiftctl: flow scaling is compiled out of the BPF "
                        "module for now (verifier limit); use --fs-range 0\n");
        return -1;
    }
    if (o->cfg.trace_every && !o->trace_path) {
        fprintf(stderr, "swiftctl: --trace-every needs --trace FILE\n");
        return -1;
    }
    return 0;
}

// Applies one parsed option. Returns 0, -1 on error, or 1 for --help.
static int apply_opt(int c, const char *arg, struct swiftctl_opts *o)
{
    struct swift_params *p = &o->cfg.p;
    double v = 0;
    switch (c) {
    case O_BASE:      if (parse_num(arg, 1, 1e6, &v)) return -1; p->base_target_us = v; break;
    case O_HOPSCALE:  if (parse_num(arg, 0, 1e5, &v)) return -1; p->hop_scale_us = v; break;
    case O_HOPS:      if (parse_num(arg, 0, 255, &v)) return -1; p->default_hops = v; break;
    case O_HOP:       return parse_hop(arg, o);
    case O_FSRANGE:   if (parse_num(arg, 0, 1e6, &v)) return -1; p->fs_range_us = v; break;
    case O_FSMIN:     if (parse_num(arg, 0.001, 1e5, &v)) return -1; p->fs_min_cwnd_fp = FPV(v); break;
    case O_FSMAX:     if (parse_num(arg, 0.001, 1e5, &v)) return -1; p->fs_max_cwnd_fp = FPV(v); break;
    case O_AI:        if (parse_num(arg, 0.001, 1000, &v)) return -1; p->ai_fp = FPV(v); break;
    case O_BETA:      if (parse_num(arg, 0.001, 1, &v)) return -1; p->beta_fp = FPV(v); break;
    case O_MDF:       if (parse_num(arg, 0.001, 0.999, &v)) return -1; p->max_mdf_fp = FPV(v); break;
    case O_MINCWND:   if (parse_num(arg, 0.0001, 1e5, &v)) return -1; p->min_cwnd_fp = FPV(v); break;
    case O_MAXCWND:   if (parse_num(arg, 1, 1e5, &v)) return -1; p->max_cwnd_fp = FPV(v); break;
    case O_INITCWND:  if (parse_num(arg, 0.0001, 1e5, &v)) return -1; p->init_cwnd_fp = FPV(v); break;
    case O_RETX:      if (parse_num(arg, 1, 1000, &v)) return -1; p->retx_reset = v; break;
    case O_MEDIAN:    if (parse_num(arg, 0, SW_RING_SIZE, &v)) return -1; p->median_cap = v; break;
    case O_REORDER:   if (parse_num(arg, 1, 255, &v)) return -1; p->reorder_k = v; break;
    case O_PACE:      if (parse_num(arg, 0, 1000, &v)) return -1; p->pace_gain_pct = v; break;
    case O_PACE_MINRTT: p->pace_rtt_min = 1; break;
    case O_PACE_GAP:  if (parse_num(arg, 0, 1e7, &v)) return -1; p->max_pace_gap_us = v; break;
    case O_TRACE_EVERY: if (parse_num(arg, 0, 1e9, &v)) return -1; o->cfg.trace_every = v; break;
    case O_NO_EVENTS: o->cfg.trace_events = 0; break;
    case O_OUT:       o->trace_path = arg; break;
    case O_STATS:     o->stats_path = arg; break;
    case O_DURATION:  if (parse_num(arg, 0, 1e7, &v)) return -1; o->duration_s = v; break;
    case O_VERBOSE:   o->verbose = 1; break;
    case O_HELP:      usage(stdout); return 1;
    default:          usage(stderr); return -1;
    }
    return 0;
}

int swiftctl_parse(int argc, char **argv, struct swiftctl_opts *o)
{
    int c, rc;
    set_defaults(o);
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        rc = apply_opt(c, optarg, o);
        if (rc == 1)
            exit(0);
        if (rc < 0)
            return -1;
    }
    sw_params_finalize(&o->cfg.p);
    if (optind < argc) {
        fprintf(stderr, "swiftctl: unexpected argument '%s'\n", argv[optind]);
        return -1;
    }
    return validate(o);
}

void swiftctl_print_cfg(const struct swiftctl_opts *o, FILE *out)
{
    const struct swift_params *p = &o->cfg.p;
    fprintf(out,
            "base_target_us=%u hop_scale_us=%u default_hops=%u fs_range_us=%u "
            "fs_min_cwnd=%.3f fs_max_cwnd=%.1f ai=%.3f beta=%.3f max_mdf=%.3f "
            "min_cwnd=%.4f max_cwnd=%.0f init_cwnd=%.2f retx_reset=%u "
            "median_cap=%u reorder_k=%u pace_gain_pct=%u pace_rtt_min=%u max_pace_gap_us=%u "
            "trace_every=%u\n",
            p->base_target_us, p->hop_scale_us, p->default_hops, p->fs_range_us,
            UNFP(p->fs_min_cwnd_fp), UNFP(p->fs_max_cwnd_fp), UNFP(p->ai_fp),
            UNFP(p->beta_fp), UNFP(p->max_mdf_fp), UNFP(p->min_cwnd_fp),
            UNFP(p->max_cwnd_fp), UNFP(p->init_cwnd_fp), p->retx_reset,
            p->median_cap, p->reorder_k, p->pace_gain_pct, p->pace_rtt_min,
            p->max_pace_gap_us, o->cfg.trace_every);
}
