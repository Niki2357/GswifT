// Incast aggregator: opens N connections spread over one or more workers,
// then repeatedly asks every connection for B bytes at the same moment and
// records each flow's completion time. Runs on the receiver.
//
//   incast_agg -s 10.10.1.1:6000 -n 128 -b 65536 -r 200 -o fct.csv
//   incast_agg -s 10.10.1.1:6000 -n 1 -b 64 -t 30 -o rpc.csv   (latency probe)
//
// Output CSV: round,flow,fct_us. Round completion time = max over the round.

#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <sys/epoll.h>

#include "incast_common.h"

#define MAX_FLOWS 4096
#define MAX_SERVERS 64
#define ROUND_TIMEOUT_NS (20LL * 1000000000LL)

struct opts {
    struct sockaddr_in servers[MAX_SERVERS];
    int n_servers, flows, warmup;
    uint32_t bytes;
    long rounds;
    double duration_s;
    long gap_us;
    const char *out;
};

struct flow {
    int fd;
    uint32_t left;
    int64_t fct_ns;
};

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int parse_servers(char *list, struct opts *o)
{
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        char *colon = strrchr(tok, ':');
        if (!colon || o->n_servers >= MAX_SERVERS)
            return -1;
        *colon = '\0';
        struct sockaddr_in *a = &o->servers[o->n_servers++];
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, tok, &a->sin_addr) != 1)
            return -1;
    }
    return o->n_servers ? 0 : -1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: incast_agg -s IP:PORT[,IP:PORT...] [-n flows] [-b bytes]\n"
            "                  [-r rounds | -t seconds] [-g gap_us] [-w warmup]\n"
            "                  -o out.csv\n");
}

static int parse(int argc, char **argv, struct opts *o)
{
    int c;
    memset(o, 0, sizeof(*o));
    o->flows = 1, o->bytes = 64, o->rounds = 100, o->warmup = 5;
    while ((c = getopt(argc, argv, "s:n:b:r:t:g:w:o:h")) != -1) {
        switch (c) {
        case 's': if (parse_servers(optarg, o)) return -1; break;
        case 'n': o->flows = atoi(optarg); break;
        case 'b': o->bytes = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'r': o->rounds = atol(optarg); break;
        case 't': o->duration_s = atof(optarg); o->rounds = 0; break;
        case 'g': o->gap_us = atol(optarg); break;
        case 'w': o->warmup = atoi(optarg); break;
        case 'o': o->out = optarg; break;
        default: return -1;
        }
    }
    if (!o->n_servers || !o->out || o->flows < 1 || o->flows > MAX_FLOWS ||
        o->bytes < 1 || o->bytes > INCAST_MAX_RESP || o->warmup < 0 ||
        o->gap_us < 0 || (o->rounds <= 0 && o->duration_s <= 0))
        return -1;
    return 0;
}

static void connect_all(const struct opts *o, struct flow *f, int ep)
{
    for (int i = 0; i < o->flows; i++) {
        const struct sockaddr_in *a = &o->servers[i % o->n_servers];
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            incast_die("socket");
        if (connect(fd, (const struct sockaddr *)a, sizeof(*a)) < 0)
            incast_die("connect");
        incast_set_nodelay(fd);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
            incast_die("epoll_ctl");
        f[i].fd = fd;
    }
}

// Drain whatever is readable on flow i. Returns 1 when the flow just finished.
static int drain(struct flow *fl, char *buf, size_t buflen, int64_t t0)
{
    while (fl->left) {
        size_t want = fl->left < buflen ? fl->left : buflen;
        ssize_t n = read(fl->fd, buf, want);
        if (n > 0) {
            fl->left -= (uint32_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        fprintf(stderr, "incast_agg: connection closed mid-round\n");
        exit(1);
    }
    fl->fct_ns = incast_now_ns() - t0;
    return 1;
}

// One synchronized round. Returns the round completion time in ns.
static int64_t run_round(const struct opts *o, struct flow *f, int ep,
                         uint64_t round, char *buf, size_t buflen)
{
    struct incast_req req = {INCAST_MAGIC, o->bytes, round};
    struct epoll_event evs[256];
    int done = 0;
    int64_t t0 = incast_now_ns();

    for (int i = 0; i < o->flows; i++) {
        f[i].left = o->bytes;
        f[i].fct_ns = 0;
        if (incast_write_full(f[i].fd, &req, sizeof(req)))
            incast_die("write request");
    }
    while (done < o->flows) {
        int n = epoll_wait(ep, evs, 256, 1000);
        if (n < 0 && errno != EINTR)
            incast_die("epoll_wait");
        for (int k = 0; k < n; k++) {
            struct flow *fl = &f[evs[k].data.u32];
            if (fl->left && drain(fl, buf, buflen, t0))
                done++;
        }
        if (incast_now_ns() - t0 > ROUND_TIMEOUT_NS) {
            fprintf(stderr, "incast_agg: round %llu timed out (%d/%d done)\n",
                    (unsigned long long)round, done, o->flows);
            exit(1);
        }
    }
    return incast_now_ns() - t0;
}

static void sleep_us(long us)
{
    if (us <= 0)
        return;
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    struct opts o;
    if (parse(argc, argv, &o)) {
        usage();
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    FILE *out = fopen(o.out, "w");
    struct flow *f = calloc((size_t)o.flows, sizeof(*f));
    size_t buflen = 1 << 20;
    char *buf = malloc(buflen);
    int ep = epoll_create1(0);
    if (!out || !f || !buf || ep < 0)
        incast_die("setup");
    setvbuf(out, NULL, _IOFBF, 1 << 22);
    fprintf(out, "round,flow,fct_us\n");

    connect_all(&o, f, ep);
    int64_t end = o.duration_s > 0
                      ? incast_now_ns() + (int64_t)(o.duration_s * 1e9) : 0;
    long measured = 0;
    double qct_sum = 0;
    for (uint64_t r = 0; !stop; r++) {
        int64_t qct = run_round(&o, f, ep, r, buf, buflen);
        if ((long)r >= o.warmup) {
            for (int i = 0; i < o.flows; i++)
                fprintf(out, "%llu,%d,%.3f\n", (unsigned long long)r, i,
                        f[i].fct_ns / 1e3);
            measured++;
            qct_sum += qct / 1e3;
        }
        if (o.rounds > 0 && measured >= o.rounds)
            break;
        if (end && incast_now_ns() >= end)
            break;
        sleep_us(o.gap_us);
    }
    fclose(out);
    fprintf(stderr, "incast_agg: %ld rounds, %d flows x %u B, mean QCT %.1f us\n",
            measured, o.flows, o.bytes, measured ? qct_sum / measured : 0.0);
    return 0;
}
