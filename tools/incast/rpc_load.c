// Open-loop RPC workload generator: Poisson arrivals, response sizes drawn
// from an empirical CDF, served by incast_worker over a pool of persistent
// connections (one outstanding RPC per connection, as TCP RPC stacks do).
// Runs on the receiver, so responses cross the bottleneck.
//
//   rpc_load -s 10.10.1.1:6000 -c 64 -l 0.5 -R 10e9 -f websearch.cdf -t 20 -o rpc.csv
//
// -l LOAD is the offered response load as a fraction of -R RATE (bits/s).
// Latency is from the scheduled arrival time to the last response byte, so it
// includes waiting for a free connection. Output CSV: size,latency_us,wait_us.
//
// CDF file: lines "<bytes> <cumulative probability>", increasing, last = 1.
// Sizes are interpolated linearly between points.

#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <sys/epoll.h>

#include "incast_common.h"

#define MAX_CONNS 1024
#define MAX_CDF 256
#define MAX_PENDING (1 << 20)

struct cdf {
    double bytes[MAX_CDF], p[MAX_CDF];
    int n;
    double mean;
};

struct conn {
    int fd;
    uint32_t left, size;
    int64_t t_arrival, t_sent;
};

struct pending {
    int64_t t_arrival;
    uint32_t size;
};

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int load_cdf(const char *path, struct cdf *c)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    c->n = 0;
    while (c->n < MAX_CDF &&
           fscanf(f, "%lf %lf", &c->bytes[c->n], &c->p[c->n]) == 2)
        c->n++;
    fclose(f);
    if (c->n < 2 || fabs(c->p[c->n - 1] - 1.0) > 1e-9)
        return -1;
    c->mean = 0;
    for (int i = 1; i < c->n; i++) {
        if (c->p[i] < c->p[i - 1] || c->bytes[i] < c->bytes[i - 1])
            return -1;
        c->mean += (c->p[i] - c->p[i - 1]) * (c->bytes[i] + c->bytes[i - 1]) / 2;
    }
    return 0;
}

static uint32_t sample_size(const struct cdf *c, double u)
{
    for (int i = 1; i < c->n; i++) {
        if (u <= c->p[i]) {
            double span = c->p[i] - c->p[i - 1];
            double frac = span > 0 ? (u - c->p[i - 1]) / span : 1.0;
            double b = c->bytes[i - 1] + frac * (c->bytes[i] - c->bytes[i - 1]);
            return b < 1 ? 1 : (uint32_t)b;
        }
    }
    return (uint32_t)c->bytes[c->n - 1];
}

static double uniform(void) { return (random() + 1.0) / (RAND_MAX + 2.0); }

static void usage(void)
{
    fprintf(stderr,
            "usage: rpc_load -s IP:PORT -f CDF -l LOAD -R RATE_BPS -t SECONDS -o OUT.csv\n"
            "                [-c CONNS] [-w WARMUP_S] [-S SEED]\n");
}

static void send_req(struct conn *c, uint32_t size, int64_t t_arrival)
{
    struct incast_req req = {INCAST_MAGIC, size, 0};
    c->size = c->left = size;
    c->t_arrival = t_arrival;
    c->t_sent = incast_now_ns();
    if (incast_write_full(c->fd, &req, sizeof(req)))
        incast_die("write request");
}

int main(int argc, char **argv)
{
    const char *server = NULL, *cdf_path = NULL, *out_path = NULL;
    double load = 0, rate_bps = 10e9, dur = 0, warm = 1;
    int nconn = 64, ch;
    long seed = 1;
    while ((ch = getopt(argc, argv, "s:f:l:R:t:o:c:w:S:h")) != -1) {
        switch (ch) {
        case 's': server = optarg; break;
        case 'f': cdf_path = optarg; break;
        case 'l': load = atof(optarg); break;
        case 'R': rate_bps = atof(optarg); break;
        case 't': dur = atof(optarg); break;
        case 'o': out_path = optarg; break;
        case 'c': nconn = atoi(optarg); break;
        case 'w': warm = atof(optarg); break;
        case 'S': seed = atol(optarg); break;
        default: usage(); return ch == 'h' ? 0 : 2;
        }
    }
    struct cdf cdf;
    char ip[64];
    const char *colon = server ? strchr(server, ':') : NULL;
    if (!colon || !cdf_path || !out_path || load <= 0 || load > 2 || dur <= 0 ||
        rate_bps <= 0 || nconn < 1 || nconn > MAX_CONNS || warm < 0 ||
        (size_t)(colon - server) >= sizeof(ip)) {
        usage();
        return 2;
    }
    if (load_cdf(cdf_path, &cdf)) {
        fprintf(stderr, "rpc_load: bad CDF file %s\n", cdf_path);
        return 2;
    }
    memcpy(ip, server, colon - server);
    ip[colon - server] = '\0';
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(atoi(colon + 1))};
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        fprintf(stderr, "rpc_load: bad address %s\n", ip);
        return 2;
    }
    srandom((unsigned)seed);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    // Mean inter-arrival so that mean_size * arrivals/s = load * rate.
    double lambda = load * rate_bps / 8.0 / cdf.mean;  // RPCs per second
    double mean_gap_ns = 1e9 / lambda;

    FILE *out = fopen(out_path, "w");
    struct conn *conns = calloc((size_t)nconn, sizeof(*conns));
    int *idle = malloc(sizeof(int) * (size_t)nconn);
    struct pending *pq = malloc(sizeof(*pq) * MAX_PENDING);
    char *buf = malloc(1 << 20);
    int ep = epoll_create1(0);
    if (!out || !conns || !idle || !pq || !buf || ep < 0)
        incast_die("setup");
    setvbuf(out, NULL, _IOFBF, 1 << 22);
    fprintf(out, "size,latency_us,wait_us\n");

    int n_idle = 0;
    for (int i = 0; i < nconn; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
            incast_die("connect");
        incast_set_nodelay(fd);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
            incast_die("epoll_ctl");
        conns[i].fd = fd;
        idle[n_idle++] = i;
    }

    int64_t t0 = incast_now_ns();
    int64_t t_warm = t0 + (int64_t)(warm * 1e9);
    int64_t t_end = t_warm + (int64_t)(dur * 1e9);
    int64_t next = t0 + (int64_t)(-log(uniform()) * mean_gap_ns);
    long pq_head = 0, pq_tail = 0, done = 0, issued = 0, dropped = 0;
    double bytes_done = 0;
    struct epoll_event evs[256];

    while (!stop) {
        int64_t now = incast_now_ns();
        // Generate arrivals due by now; stop generating at t_end.
        while (next <= now && next < t_end) {
            uint32_t sz = sample_size(&cdf, uniform());
            if (n_idle) {
                send_req(&conns[idle[--n_idle]], sz, next);
            } else if (pq_tail - pq_head < MAX_PENDING) {
                pq[pq_tail % MAX_PENDING] = (struct pending){next, sz};
                pq_tail++;
            } else {
                dropped++;
            }
            issued++;
            next += (int64_t)(-log(uniform()) * mean_gap_ns);
        }
        int busy = nconn - n_idle;
        if (next >= t_end && busy == 0 && pq_head == pq_tail)
            break;
        // Wait for completions, or until the next arrival.
        int timeout_ms = 0;
        if (next < t_end && next > now)
            timeout_ms = (int)((next - now) / 1000000);
        else if (next >= t_end)
            timeout_ms = 100;
        int n = epoll_wait(ep, evs, 256, timeout_ms);
        if (n < 0 && errno != EINTR)
            incast_die("epoll_wait");
        for (int k = 0; k < n; k++) {
            int i = (int)evs[k].data.u32;
            struct conn *c = &conns[i];
            while (c->left) {
                ssize_t r = read(c->fd, buf, c->left < (1u << 20) ? c->left : (1u << 20));
                if (r > 0) {
                    c->left -= (uint32_t)r;
                    continue;
                }
                if (r < 0 && (errno == EAGAIN || errno == EINTR))
                    break;
                fprintf(stderr, "rpc_load: connection closed\n");
                return 1;
            }
            if (c->left)
                continue;
            int64_t t = incast_now_ns();
            if (c->t_arrival >= t_warm && c->t_arrival < t_end) {
                fprintf(out, "%u,%.3f,%.3f\n", c->size, (t - c->t_arrival) / 1e3,
                        (c->t_sent - c->t_arrival) / 1e3);
                done++;
                bytes_done += c->size;
            }
            if (pq_head < pq_tail) {
                struct pending p = pq[pq_head % MAX_PENDING];
                pq_head++;
                send_req(c, p.size, p.t_arrival);
            } else {
                idle[n_idle++] = i;
            }
        }
        if (incast_now_ns() > t_end + 30LL * 1000000000LL) {
            fprintf(stderr, "rpc_load: drain timeout, %d busy\n", nconn - n_idle);
            break;
        }
    }
    fclose(out);
    fprintf(stderr,
            "rpc_load: mean size %.0f B, lambda %.0f/s, issued %ld, measured %ld, "
            "dropped %ld, achieved %.2f Gb/s\n",
            cdf.mean, lambda, issued, done, dropped, bytes_done * 8 / dur / 1e9);
    return 0;
}
