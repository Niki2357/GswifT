// Incast worker: answers each request with the requested number of bytes.
// Runs on sender nodes. The congestion control under test is set here, since
// the worker is the data sender.
//
//   incast_worker -p 6000 -C swift

#include <arpa/inet.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>

#include "incast_common.h"

static const char *g_cc;
static char *g_payload;

static void *serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct incast_req req;

    incast_set_cc(fd, g_cc);
    incast_set_nodelay(fd);
    while (incast_read_full(fd, &req, sizeof(req)) == 0) {
        if (req.magic != INCAST_MAGIC || req.resp_bytes > INCAST_MAX_RESP) {
            fprintf(stderr, "incast_worker: bad request, closing\n");
            break;
        }
        if (incast_write_full(fd, g_payload, req.resp_bytes))
            break;
    }
    close(fd);
    return NULL;
}

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0)
        incast_die("socket");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    incast_set_cc(fd, g_cc);  // inherited by accepted sockets
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        incast_die("bind");
    if (listen(fd, 4096) < 0)
        incast_die("listen");
    return fd;
}

int main(int argc, char **argv)
{
    int port = 6000, c;
    while ((c = getopt(argc, argv, "p:C:h")) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'C': g_cc = optarg; break;
        default:
            fprintf(stderr, "usage: incast_worker [-p port] [-C cc]\n");
            return c == 'h' ? 0 : 2;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "incast_worker: bad port\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g_payload = calloc(1, INCAST_MAX_RESP);
    if (!g_payload)
        incast_die("calloc");

    int lfd = listen_on(port);
    fprintf(stderr, "incast_worker: port %d cc %s\n", port, g_cc ? g_cc : "default");
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            incast_die("accept");
        }
        pthread_t t;
        if (pthread_create(&t, NULL, serve, (void *)(intptr_t)fd) != 0) {
            fprintf(stderr, "incast_worker: pthread_create failed\n");
            close(fd);
            continue;
        }
        pthread_detach(t);
    }
}
