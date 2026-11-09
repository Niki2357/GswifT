// swiftctl: load the Swift struct_ops congestion control, configure it, and
// stream its telemetry to CSV until SIGINT/SIGTERM or --duration elapses.
//
//   sudo ./swiftctl --base-target 100 --trace /mydata/trace.csv --trace-every 8
//
// The congestion control is registered as "swift" for as long as swiftctl
// runs, and unregistered when it exits.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "swift.skel.h"
#include "swiftctl_args.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int libbpf_log(enum libbpf_print_level lvl, const char *fmt, va_list ap)
{
    if (lvl == LIBBPF_DEBUG)
        return 0;
    return vfprintf(stderr, fmt, ap);
}

struct trace_ctx {
    FILE *out;
    unsigned long long records;
};

static int on_event(void *ctx, void *data, size_t len)
{
    struct trace_ctx *t = ctx;
    const struct swift_event *e = data;
    if (len < sizeof(*e))
        return 0;
    fprintf(t->out, "%llu,%u,%u,%u,%u,%u,%u,%.5f,%u,%u,%u,%llu,%u,%u,%u\n",
            (unsigned long long)e->ts_ns, e->sport, e->dport, e->type,
            e->action, e->rtt_us, e->srtt_us,
            (double)e->cwnd_fp / SW_FP_ONE, e->snd_cwnd, e->target_us,
            e->delay_us, (unsigned long long)e->pacing_Bps, e->inflight,
            e->acked, e->retx_cnt);
    t->records++;
    return 0;
}

static int configure(struct swift_bpf *skel, const struct swiftctl_opts *o)
{
    __u32 zero = 0;
    if (bpf_map_update_elem(bpf_map__fd(skel->maps.cfg_map), &zero, &o->cfg,
                            BPF_ANY)) {
        fprintf(stderr, "swiftctl: writing config map: %s\n", strerror(errno));
        return -1;
    }
    for (int i = 0; i < o->n_hops; i++) {
        if (bpf_map_update_elem(bpf_map__fd(skel->maps.hops_map),
                                &o->hops[i].daddr, &o->hops[i].hops, BPF_ANY)) {
            fprintf(stderr, "swiftctl: writing hops map: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void write_stats(const struct swift_bpf *skel, unsigned long long records,
                        FILE *out)
{
    const struct swift_bpf__bss *b = skel->bss;
    fprintf(out,
            "n_init=%llu\nn_acks=%llu\nn_samples=%llu\nn_increase=%llu\n"
            "n_decrease=%llu\nn_hold=%llu\nn_rto=%llu\nn_recovery=%llu\n"
            "n_frac_acks=%llu\nn_ev_dropped=%llu\nn_no_cfg=%llu\n"
            "trace_records=%llu\n",
            (unsigned long long)b->n_init, (unsigned long long)b->n_acks,
            (unsigned long long)b->n_samples, (unsigned long long)b->n_increase,
            (unsigned long long)b->n_decrease, (unsigned long long)b->n_hold,
            (unsigned long long)b->n_rto, (unsigned long long)b->n_recovery,
            (unsigned long long)b->n_frac_acks,
            (unsigned long long)b->n_ev_dropped,
            (unsigned long long)b->n_no_cfg, records);
}

static int run_loop(struct ring_buffer *rb, double duration_s)
{
    double end = duration_s > 0 ? mono_s() + duration_s : 0;
    while (!stop && (end == 0 || mono_s() < end)) {
        int err = ring_buffer__poll(rb, 100);
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "swiftctl: ring buffer poll: %d\n", err);
            return -1;
        }
    }
    ring_buffer__consume(rb);
    return 0;
}

static FILE *open_trace(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "swiftctl: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 1 << 22);
    fprintf(f, "ts_ns,sport,dport,type,action,rtt_us,srtt_us,cwnd,snd_cwnd,"
               "target_us,delay_us,pacing_Bps,inflight,acked,retx\n");
    return f;
}

int main(int argc, char **argv)
{
    struct swiftctl_opts o;
    struct trace_ctx tctx = {0};
    struct swift_bpf *skel = NULL;
    struct bpf_link *link = NULL;
    struct ring_buffer *rb = NULL;
    int rc = 1;

    if (swiftctl_parse(argc, argv, &o))
        return 2;
    libbpf_set_print(libbpf_log);
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);  // needed on kernels before 5.11

    skel = swift_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "swiftctl: failed to load BPF object (run as root?)\n");
        goto out;
    }
    if (configure(skel, &o))
        goto out;
    if (o.trace_path && !(tctx.out = open_trace(o.trace_path)))
        goto out;
    if (tctx.out) {
        rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, &tctx,
                              NULL);
        if (!rb) {
            fprintf(stderr, "swiftctl: ring_buffer__new failed\n");
            goto out;
        }
    }
    link = bpf_map__attach_struct_ops(skel->maps.swift);
    if (!link) {
        fprintf(stderr,
                "swiftctl: attach failed: %s\n"
                "  If 'swift' is already registered from an earlier run:\n"
                "  sudo bpftool struct_ops unregister name swift\n",
                strerror(errno));
        goto out;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "swiftctl: 'swift' registered. ");
    swiftctl_print_cfg(&o, stderr);

    if (rb)
        rc = run_loop(rb, o.duration_s) ? 1 : 0;
    else {
        double end = o.duration_s > 0 ? mono_s() + o.duration_s : 0;
        while (!stop && (end == 0 || mono_s() < end))
            usleep(100000);
        rc = 0;
    }

    write_stats(skel, tctx.records, stderr);
    if (o.stats_path) {
        FILE *f = fopen(o.stats_path, "w");
        if (f) {
            swiftctl_print_cfg(&o, f);
            write_stats(skel, tctx.records, f);
            fclose(f);
        } else {
            fprintf(stderr, "swiftctl: cannot write %s\n", o.stats_path);
        }
    }
out:
    bpf_link__destroy(link);
    ring_buffer__free(rb);
    if (tctx.out)
        fclose(tctx.out);
    swift_bpf__destroy(skel);
    return rc;
}
