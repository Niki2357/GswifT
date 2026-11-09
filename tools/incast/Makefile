# Incast / RPC-latency tool. Linux only (epoll, TCP_CONGESTION).
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11

.PHONY: all clean
all: incast_worker incast_agg

incast_worker: incast_worker.c incast_common.h
	$(CC) $(CFLAGS) -o $@ incast_worker.c -lpthread

incast_agg: incast_agg.c incast_common.h
	$(CC) $(CFLAGS) -o $@ incast_agg.c

clean:
	rm -f incast_worker incast_agg
