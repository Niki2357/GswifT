// Command-line options for swiftctl.
#ifndef SWIFTCTL_ARGS_H
#define SWIFTCTL_ARGS_H

#include <stdint.h>
#include <stdio.h>

#include "swift_types.h"

#define SWIFTCTL_MAX_HOPS 64

struct hop_entry {
    uint32_t daddr;  // network byte order
    uint32_t hops;
};

struct swiftctl_opts {
    struct swift_cfg cfg;
    struct hop_entry hops[SWIFTCTL_MAX_HOPS];
    int n_hops;
    const char *trace_path;  // CSV of per-ACK records, NULL = none
    const char *stats_path;  // counters at exit, NULL = stderr only
    double duration_s;       // 0 = run until SIGINT/SIGTERM
    int verbose;
};

// Fills opts with defaults, then parses argv. Returns 0 on success, prints a
// message and returns -1 on invalid input.
int swiftctl_parse(int argc, char **argv, struct swiftctl_opts *opts);

// Human-readable summary of the effective configuration, one line.
void swiftctl_print_cfg(const struct swiftctl_opts *opts, FILE *out);

#endif
