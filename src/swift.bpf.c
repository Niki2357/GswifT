// Swift delay-based congestion control as a BPF struct_ops TCP module.
//
// The algorithm lives in swift_core.h (unit-tested in userspace). This file
// adapts it to Linux:
//   - cong_control runs Swift's per-ACK update with rate_sample.rtt_us.
//   - The fractional cwnd is kept in module state. Linux gets
//     snd_cwnd = max(1, floor(cwnd)); below 1 packet the rate is enforced
//     with sk_pacing_rate = mss * cwnd / srtt.
//   - RTO arrives as CA_EVENT_LOSS, fast recovery as set_state(Recovery).
//   - Telemetry goes to a ring buffer, read by swiftctl.
//
// Works on kernels 5.15+ (struct_ops TCP CA, ring buffer, bounded loops).

#include "vmlinux.h"
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "swift_core.h"

char _license[] SEC("license") = "GPL";

// libbpf >= 1.2 and kernel >= 6.4: link-based struct_ops, unregistered
// automatically when swiftctl exits. Older systems use the plain section.
#ifndef SWIFT_STRUCT_OPS_SEC
#define SWIFT_STRUCT_OPS_SEC ".struct_ops"
#endif

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct swift_cfg);
} cfg_map SEC(".maps");

// Destination IPv4 address (network order) -> hop count.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);
    __type(value, __u32);
} hops_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);
} events SEC(".maps");

// Global counters, read by swiftctl from .bss at exit.
__u64 n_init, n_acks, n_samples, n_increase, n_decrease, n_hold;
__u64 n_rto, n_recovery, n_frac_acks, n_ev_dropped, n_no_cfg;

static __always_inline struct tcp_sock *tcp_sk(const struct sock *sk)
{
    return (struct tcp_sock *)sk;
}

static __always_inline struct swift_state *ca_state(struct sock *sk)
{
    return (struct swift_state *)((struct inet_connection_sock *)sk)->icsk_ca_priv;
}

static __always_inline const struct swift_cfg *get_cfg(void)
{
    __u32 zero = 0;
    const struct swift_cfg *c = bpf_map_lookup_elem(&cfg_map, &zero);
    if (!c)
        __sync_fetch_and_add(&n_no_cfg, 1);
    return c;
}

static __always_inline __u64 srtt_of(const struct tcp_sock *tp)
{
    return tp->srtt_us >> 3;
}

// Push the module's fractional cwnd into Linux: integer cwnd plus pacing.
static __always_inline __u64 apply(struct sock *sk, const struct swift_params *p,
                                   const struct swift_state *st)
{
    struct tcp_sock *tp = tcp_sk(sk);
    __u64 srtt = srtt_of(tp);
    if (srtt == 0)
        srtt = st->delay_us;
    __u64 rate = sw_pacing_Bps(p, st->cwnd_fp, tp->mss_cache, srtt);
    __u64 max = sk->sk_max_pacing_rate;

    tp->snd_cwnd = sw_snd_cwnd(st->cwnd_fp);
    sk->sk_pacing_status = SK_PACING_NEEDED;
    sk->sk_pacing_rate = (rate == 0 || rate > max) ? max : rate;
    return sk->sk_pacing_rate;
}

static __always_inline void emit(struct sock *sk, const struct swift_state *st,
                                 __u8 type, __u8 action, long rtt_us,
                                 __u32 acked, __u64 pacing)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct swift_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        __sync_fetch_and_add(&n_ev_dropped, 1);
        return;
    }
    e->ts_ns = bpf_ktime_get_ns();
    e->cwnd_fp = st->cwnd_fp;
    e->pacing_Bps = pacing;
    e->rtt_us = rtt_us > 0 ? (__u32)rtt_us : 0;
    e->srtt_us = (__u32)srtt_of(tp);
    e->delay_us = st->delay_us;
    e->target_us = st->target_us;
    e->snd_cwnd = tp->snd_cwnd;
    e->inflight = tp->packets_out;
    e->acked = acked;
    e->sport = sk->__sk_common.skc_num;
    e->dport = bpf_ntohs(sk->__sk_common.skc_dport);
    e->retx_cnt = st->retx_cnt;
    e->type = type;
    e->action = action;
    bpf_ringbuf_submit(e, 0);
}

static __always_inline void count_action(int act)
{
    if (act == SW_INCREASE)
        __sync_fetch_and_add(&n_increase, 1);
    else if (act == SW_DECREASE)
        __sync_fetch_and_add(&n_decrease, 1);
    else if (act == SW_HOLD)
        __sync_fetch_and_add(&n_hold, 1);
}

SEC("struct_ops/swift_init")
void BPF_PROG(swift_init, struct sock *sk)
{
    const struct swift_cfg *c = get_cfg();
    if (!c)
        return;
    __u32 daddr = sk->__sk_common.skc_daddr;
    __u32 *h = bpf_map_lookup_elem(&hops_map, &daddr);
    struct swift_state st;

    sw_init(&c->p, &st, h ? *h : c->p.default_hops);
    st.target_us = sw_target_us(&c->p, st.cwnd_fp, st.hops);
    __builtin_memcpy(ca_state(sk), &st, sizeof(st));
    __u64 pacing = apply(sk, &c->p, &st);
    __sync_fetch_and_add(&n_init, 1);
    if (c->trace_events)
        emit(sk, &st, SW_EV_INIT, SW_NONE, 0, 0, pacing);
}

SEC("struct_ops/swift_cong_control")
void BPF_PROG(swift_cong_control, struct sock *sk, const struct rate_sample *rs)
{
    const struct swift_cfg *c = get_cfg();
    if (!c)
        return;
    struct tcp_sock *tp = tcp_sk(sk);
    struct swift_state st;
    long rtt = rs->rtt_us;
    __u32 acked = rs->acked_sacked;

    __builtin_memcpy(&st, ca_state(sk), sizeof(st));
    int act = sw_on_ack(&c->p, &st, tp->tcp_mstamp, rtt, srtt_of(tp), acked);
    st.ack_cnt++;
    __builtin_memcpy(ca_state(sk), &st, sizeof(st));
    __u64 pacing = apply(sk, &c->p, &st);

    __sync_fetch_and_add(&n_acks, 1);
    if (rtt > 0)
        __sync_fetch_and_add(&n_samples, 1);
    if (st.cwnd_fp < SW_FP_ONE)
        __sync_fetch_and_add(&n_frac_acks, 1);
    count_action(act);

    if (c->trace_events && act == SW_DECREASE)
        emit(sk, &st, SW_EV_DECREASE, act, rtt, acked, pacing);
    else if (c->trace_every && st.ack_cnt % c->trace_every == 0)
        emit(sk, &st, SW_EV_ACK, act, rtt, acked, pacing);
}

SEC("struct_ops/swift_cwnd_event")
void BPF_PROG(swift_cwnd_event, struct sock *sk, enum tcp_ca_event ev)
{
    if (ev != CA_EVENT_LOSS)
        return;
    const struct swift_cfg *c = get_cfg();
    if (!c)
        return;
    struct tcp_sock *tp = tcp_sk(sk);
    struct swift_state st;

    __builtin_memcpy(&st, ca_state(sk), sizeof(st));
    sw_on_rto(&c->p, &st, tp->tcp_mstamp, srtt_of(tp));
    __builtin_memcpy(ca_state(sk), &st, sizeof(st));
    __u64 pacing = apply(sk, &c->p, &st);
    __sync_fetch_and_add(&n_rto, 1);
    if (c->trace_events)
        emit(sk, &st, SW_EV_RTO, SW_DECREASE, 0, 0, pacing);
}

SEC("struct_ops/swift_set_state")
void BPF_PROG(swift_set_state, struct sock *sk, __u8 new_state)
{
    if (new_state != TCP_CA_Recovery)
        return;
    const struct swift_cfg *c = get_cfg();
    if (!c)
        return;
    struct tcp_sock *tp = tcp_sk(sk);
    struct swift_state st;

    __builtin_memcpy(&st, ca_state(sk), sizeof(st));
    sw_on_recovery(&c->p, &st, tp->tcp_mstamp, srtt_of(tp));
    __builtin_memcpy(ca_state(sk), &st, sizeof(st));
    __u64 pacing = apply(sk, &c->p, &st);
    __sync_fetch_and_add(&n_recovery, 1);
    if (c->trace_events)
        emit(sk, &st, SW_EV_RECOVERY, SW_DECREASE, 0, 0, pacing);
}

// Linux calls this on loss to set snd_ssthresh. Swift has no slow start, so
// ssthresh is informational only; cwnd is owned by cong_control.
SEC("struct_ops/swift_ssthresh")
__u32 BPF_PROG(swift_ssthresh, struct sock *sk)
{
    __u32 w = tcp_sk(sk)->snd_cwnd;
    return w < 2 ? 2 : w;
}

// No undo: the next ACK rewrites snd_cwnd from module state anyway.
SEC("struct_ops/swift_undo_cwnd")
__u32 BPF_PROG(swift_undo_cwnd, struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

SEC(SWIFT_STRUCT_OPS_SEC)
struct tcp_congestion_ops swift = {
    .init = (void *)swift_init,
    .cong_control = (void *)swift_cong_control,
    .cwnd_event = (void *)swift_cwnd_event,
    .set_state = (void *)swift_set_state,
    .ssthresh = (void *)swift_ssthresh,
    .undo_cwnd = (void *)swift_undo_cwnd,
    .name = "swift",
};
