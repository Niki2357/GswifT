// Per-packet random path choice for the MSwift spraying experiment.
//
// A cls_bpf classifier (non direct-action) on the receiver's bottleneck
// qdisc. Each TCP data packet to a port in [DPORT_LO, DPORT_HI] goes to SLOW_CLASSID (the slow,
// netem-delayed path) with probability THRESH / 2^32; everything else
// returns 0, so the qdisc's default band (the shared FIFO) takes it.
//
// Runs after receive offloads, so with GRO off every wire packet is chosen
// independently, which is MSwift's model. Built per run with the threshold
// as a constant (scripts/bottleneck.sh does this):
//
//   clang -O2 -target bpf -DTHRESH=429496730 -DDPORT_LO=5201 -DDPORT_HI=5264 \
//         -DSLOW_CLASSID=0x20001 -c spray_cls.bpf.c -o spray_cls.o

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#ifndef THRESH
#error "THRESH (probability x 2^32) is required"
#endif
#ifndef DPORT_LO
#define DPORT_LO 5201
#endif
#ifndef DPORT_HI
#define DPORT_HI DPORT_LO
#endif
#ifndef SLOW_CLASSID
#define SLOW_CLASSID 0x20001
#endif

char _license[] SEC("license") = "GPL";

// IPv4 header offset: 0 if data starts at the IP header, 14 if at Ethernet.
// After an ingress mirred redirect the MAC header is pushed back on.
static __always_inline int ip_offset(struct __sk_buff *skb)
{
    __u8 b0;
    __u16 eth_proto;
    if (bpf_skb_load_bytes(skb, 0, &b0, 1) ||
        bpf_skb_load_bytes(skb, 12, &eth_proto, 2))
        return -1;
    // Ethernet: bytes 12-13 are the EtherType. Raw IPv4: they are the first
    // two bytes of the source address (10.10 here), never 0x0800.
    if (eth_proto == bpf_htons(ETH_P_IP))
        return ETH_HLEN;
    return (b0 >> 4) == 4 ? 0 : -1;
}

SEC("classifier")
int spray(struct __sk_buff *skb)
{
    struct iphdr ip;
    __u16 dport;
    int off = ip_offset(skb);
    if (off < 0 || bpf_skb_load_bytes(skb, off, &ip, sizeof(ip)))
        return 0;
    if (ip.version != 4 || ip.protocol != IPPROTO_TCP)
        return 0;
    int l4 = off + ip.ihl * 4;
    if (bpf_skb_load_bytes(skb, l4 + 2, &dport, sizeof(dport)))
        return 0;
    __u16 dp = bpf_ntohs(dport);
    if (dp < DPORT_LO || dp > DPORT_HI)
        return 0;
    // Pure ACKs carry no data; leave them on the direct path.
    __u32 ip_len = bpf_ntohs(ip.tot_len);
    __u8 doff;
    if (bpf_skb_load_bytes(skb, l4 + 12, &doff, 1))
        return 0;
    if (ip_len <= (__u32)(ip.ihl * 4 + (doff >> 4) * 4))
        return 0;
    return bpf_get_prandom_u32() < (__u32)THRESH ? SLOW_CLASSID : 0;
}
