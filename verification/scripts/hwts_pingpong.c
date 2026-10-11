// TCP ping-pong with NIC hardware timestamps on both ends.
//
// Validates that the NIC timestamps ordinary TCP data packets (not just PTP),
// and splits each RTT into network and endpoint components without needing
// synchronized clocks: every subtraction uses timestamps from a single PHC.
//
//   client: app_rtt  = app recv - app send                (CLOCK_MONOTONIC)
//           nic_rtt  = NIC rx of reply - NIC tx of request (client PHC)
//   server: resid    = NIC tx of reply - NIC rx of request (server PHC)
//   network RTT      = nic_rtt - resid    (wire + switch, both directions)
//   client host      = app_rtt - nic_rtt  (client stack, both directions)
//
// Requires: hwstamp_ctl -i IFACE -t 1 -r 1   (HWTSTAMP_FILTER_ALL) on both nodes.
//
// Usage:  hwts_pingpong server PORT
//         hwts_pingpong client HOST PORT N [msg_bytes] > samples.csv

#include <arpa/inet.h>
#include <errno.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXMSG 65536

static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec; }

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ts_ns(&t);
}

static void die(const char *m) { perror(m); exit(1); }

static void enable_ts(int fd)
{
    int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE |
                SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0)
        die("SO_TIMESTAMPING");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
}

// Read exactly len bytes. Returns the hardware and software RX timestamps of
// the first segment of the message.
static int recv_msg(int fd, char *buf, int len, int64_t *hw, int64_t *sw)
{
    int got = 0;
    *hw = *sw = 0;
    while (got < len) {
        char ctrl[512];
        struct iovec iov = { buf + got, len - got };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = ctrl, .msg_controllen = sizeof(ctrl) };
        int n = recvmsg(fd, &m, 0);
        if (n <= 0) return -1;
        if (got == 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
                    struct scm_timestamping *t = (void *)CMSG_DATA(c);
                    *sw = ts_ns(&t->ts[0]);
                    *hw = ts_ns(&t->ts[2]);
                }
            }
        }
        got += n;
    }
    return 0;
}

// Drain the error queue until a hardware TX timestamp for byte offset `id`
// arrives. Returns 0 if none arrives within 20 ms.
static int64_t tx_hw_ts(int fd, uint32_t id, int64_t *sw)
{
    int64_t hw = 0;
    int64_t deadline = now_ns() + 20000000;
    while (!hw && now_ns() < deadline) {
        struct pollfd p = { fd, POLLPRI, 0 };
        poll(&p, 1, 1);
        for (;;) {
            char ctrl[512];
            struct msghdr m = { .msg_control = ctrl, .msg_controllen = sizeof(ctrl) };
            if (recvmsg(fd, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) break;
            struct scm_timestamping *t = NULL;
            struct sock_extended_err *e = NULL;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING)
                    t = (void *)CMSG_DATA(c);
                else if ((c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
                         (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR))
                    e = (void *)CMSG_DATA(c);
            }
            if (!t || !e || e->ee_info != SCM_TSTAMP_SND || e->ee_data != id) continue;
            if (t->ts[2].tv_sec || t->ts[2].tv_nsec) hw = ts_ns(&t->ts[2]);
            else if (sw) *sw = ts_ns(&t->ts[0]);
        }
    }
    return hw;
}

static void server(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (bind(ls, (void *)&a, sizeof(a)) < 0) die("bind");
    listen(ls, 1);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        enable_ts(fd);
        static char buf[MAXMSG];
        int len;
        uint32_t sent = 0;
        // First 4 bytes from the client carry the message length.
        if (recv(fd, &len, 4, MSG_WAITALL) != 4) { close(fd); continue; }
        for (;;) {
            int64_t rx_hw, rx_sw, tx_sw = 0;
            if (recv_msg(fd, buf, len, &rx_hw, &rx_sw) < 0) break;
            if (send(fd, buf, len, 0) != len) break;
            int64_t tx_hw = tx_hw_ts(fd, sent + len - 1, &tx_sw);
            sent += len;
            // Report server residence time back in-band, 8 bytes.
            int64_t resid = (rx_hw && tx_hw) ? tx_hw - rx_hw : -1;
            if (send(fd, &resid, 8, 0) != 8) break;
            sent += 8;
            tx_hw_ts(fd, sent - 1, NULL);  // drain
        }
        close(fd);
    }
}

static void client(const char *host, int port, int n, int len)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (void *)&a, sizeof(a)) < 0) die("connect");
    enable_ts(fd);
    send(fd, &len, 4, 0);
    tx_hw_ts(fd, 3, NULL);
    static char buf[MAXMSG];
    memset(buf, 'x', len);
    uint32_t sent = 4;
    int missing = 0;
    printf("i,app_rtt_ns,nic_rtt_ns,server_resid_ns,network_rtt_ns,client_host_ns,client_tx_stack_ns,client_rx_stack_ns\n");
    for (int i = 0; i < n; i++) {
        int64_t t0 = now_ns();
        if (send(fd, buf, len, 0) != len) die("send");
        int64_t rx_hw, rx_sw, resid, d1, d2, tx_sw = 0;
        if (recv_msg(fd, buf, len, &rx_hw, &rx_sw) < 0) die("server closed");
        int64_t t1 = now_ns();
        if (recv_msg(fd, (char *)&resid, 8, &d1, &d2) < 0) die("server closed");
        int64_t tx_hw = tx_hw_ts(fd, sent + len - 1, &tx_sw);
        sent += len;
        if (!tx_hw || !rx_hw || resid < 0) {
            if (missing++ < 5) fprintf(stderr, "missing: tx_hw=%lld rx_hw=%lld resid=%lld\n", (long long)tx_hw, (long long)rx_hw, (long long)resid);
            continue;
        }
        int64_t nic_rtt = rx_hw - tx_hw;
        // Software TX/RX timestamps are CLOCK_REALTIME; the stack-side deltas
        // below compare them with the PHC, so they are only meaningful when
        // the PHC is disciplined to CLOCK_REALTIME (phc2sys). Reported anyway.
        printf("%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", i, (long long)(t1 - t0),
               (long long)nic_rtt, (long long)resid, (long long)(nic_rtt - resid),
               (long long)(t1 - t0 - nic_rtt),
               (long long)(tx_sw ? tx_hw - tx_sw : 0),
               (long long)(rx_sw ? rx_sw - rx_hw : 0));
        usleep(100);
    }
    fprintf(stderr, "done: %d samples, %d missing hardware timestamps\n", n - missing, missing);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "server")) server(atoi(argv[2]));
    else if (argc >= 5 && !strcmp(argv[1], "client"))
        client(argv[2], atoi(argv[3]), atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 64);
    else { fprintf(stderr, "usage: see header\n"); return 1; }
    return 0;
}
