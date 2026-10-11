// Minimal struct_ops congestion control used to verify, on the target kernel,
// the three mechanisms the Swift module depends on:
//
//   1. The cong_control hook is accepted and called on every ACK.
//   2. Per-ACK RTT samples (rate_sample.rtt_us) are readable.
//   3. Writes to sk_pacing_rate / sk_pacing_status are allowed and enforced,
//      which is how a fractional cwnd will be implemented.
//
// It holds cwnd fixed and paces at PACE_BPS. If pacing works, iperf3 with
// -C cc_probe runs at about PACE_BPS instead of line rate.
//
// Build:  bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
//         clang -O2 -g -target bpf -c cc_probe.bpf.c -o cc_probe.bpf.o
// Load:   sudo bpftool struct_ops register cc_probe.bpf.o

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

#ifndef PACE_BPS
#define PACE_BPS 2000000000ULL
#endif
#ifndef FIXED_CWND
#define FIXED_CWND 200
#endif
#define SK_PACING_NEEDED 1

// Readable with: bpftool map dump name cc_probe.bss
__u64 n_init, n_cong_control, n_rtt_samples, rtt_sum_us;
__u32 rtt_min_us = ~0U, rtt_max_us;

static inline struct tcp_sock *tcp_sk(const struct sock *sk)
{
    return (struct tcp_sock *)sk;
}

SEC("struct_ops/probe_init")
void BPF_PROG(probe_init, struct sock *sk)
{
    __sync_fetch_and_add(&n_init, 1);
    sk->sk_pacing_status = SK_PACING_NEEDED;
    sk->sk_pacing_rate = PACE_BPS / 8;
    tcp_sk(sk)->snd_cwnd = FIXED_CWND;
}

SEC("struct_ops/probe_cong_control")
void BPF_PROG(probe_cong_control, struct sock *sk, const struct rate_sample *rs)
{
    __sync_fetch_and_add(&n_cong_control, 1);
    long rtt = rs->rtt_us;
    if (rtt > 0) {
        __sync_fetch_and_add(&n_rtt_samples, 1);
        __sync_fetch_and_add(&rtt_sum_us, rtt);
        if (rtt < rtt_min_us) rtt_min_us = rtt;
        if (rtt > rtt_max_us) rtt_max_us = rtt;
    }
    tcp_sk(sk)->snd_cwnd = FIXED_CWND;
    sk->sk_pacing_rate = PACE_BPS / 8;
}

SEC("struct_ops/probe_ssthresh")
__u32 BPF_PROG(probe_ssthresh, struct sock *sk)
{
    return FIXED_CWND;
}

SEC("struct_ops/probe_undo_cwnd")
__u32 BPF_PROG(probe_undo_cwnd, struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

SEC(".struct_ops")
struct tcp_congestion_ops cc_probe = {
    .init         = (void *)probe_init,
    .cong_control = (void *)probe_cong_control,
    .ssthresh     = (void *)probe_ssthresh,
    .undo_cwnd    = (void *)probe_undo_cwnd,
    .name         = "cc_probe",
};
