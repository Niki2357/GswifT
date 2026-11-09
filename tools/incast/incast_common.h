// Shared helpers for the incast worker and aggregator.
#ifndef INCAST_COMMON_H
#define INCAST_COMMON_H

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define INCAST_MAGIC 0x53574654u  // "SWFT"
#define INCAST_MAX_RESP (64u << 20)

// Aggregator -> worker: "send me resp_bytes bytes".
struct incast_req {
    uint32_t magic;
    uint32_t resp_bytes;
    uint64_t round;
};

static inline int64_t incast_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static inline void incast_die(const char *what)
{
    fprintf(stderr, "incast: %s: %s\n", what, strerror(errno));
    exit(1);
}

// Blocking read/write of exactly len bytes. Return 0, or -1 on EOF/error.
static inline int incast_read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline int incast_write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline void incast_set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Sets the congestion control on a socket. Exits on failure so a run never
// silently uses the wrong algorithm.
static inline void incast_set_cc(int fd, const char *cc)
{
    if (!cc)
        return;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, cc, strlen(cc)) < 0) {
        fprintf(stderr, "incast: TCP_CONGESTION=%s: %s (module loaded?)\n", cc,
                strerror(errno));
        exit(1);
    }
}

#endif
