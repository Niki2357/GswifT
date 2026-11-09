# Build the Swift BPF congestion control module and its loader.
#
# Needs (installed by scripts/setup_node.sh): clang, llvm, libbpf-dev,
# bpftool (linux-tools-$(uname -r)), make, gcc, pkg-config.
#
#   make            build swift.bpf.o, swift.skel.h, swiftctl
#   make check      verify the BPF object passes the verifier (bpftool load)

CLANG   ?= clang
CC      ?= gcc
BPFTOOL ?= $(shell command -v bpftool 2>/dev/null || ls /usr/lib/linux-tools/*/bpftool 2>/dev/null | head -1)

ARCH := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')

# Link-based struct_ops needs libbpf >= 1.2 and kernel >= 6.4. With it, the
# congestion control is unregistered automatically if swiftctl dies.
KVER_OK   := $(shell printf '%s\n6.4\n' "$$(uname -r | cut -d- -f1)" | sort -V | head -1 | grep -qx 6.4 && echo 1)
LIBBPF_OK := $(shell pkg-config --atleast-version=1.2 libbpf 2>/dev/null && echo 1)
ifeq ($(KVER_OK)$(LIBBPF_OK),11)
STRUCT_OPS_SEC := .struct_ops.link
else
STRUCT_OPS_SEC := .struct_ops
endif

BPF_CFLAGS = -O2 -g -target bpf -D__TARGET_ARCH_$(ARCH) -I. \
             -DSWIFT_STRUCT_OPS_SEC='"$(STRUCT_OPS_SEC)"' -Wall -Wno-unused-function
CFLAGS  ?= -O2 -g -Wall -Wextra
LIBS     = $(shell pkg-config --libs libbpf 2>/dev/null || echo -lbpf -lelf -lz)

.PHONY: all clean check info
all: swiftctl

info:
	@echo "arch=$(ARCH) struct_ops_section=$(STRUCT_OPS_SEC) bpftool=$(BPFTOOL)"
	@echo "libbpf=$$(pkg-config --modversion libbpf 2>/dev/null) kernel=$$(uname -r)"

vmlinux.h:
	@test -n "$(BPFTOOL)" || { echo "bpftool not found; run scripts/setup_node.sh"; exit 1; }
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

swift.bpf.o: swift.bpf.c swift_core.h swift_types.h vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c swift.bpf.c -o $@

swift.skel.h: swift.bpf.o
	$(BPFTOOL) gen skeleton $< name swift_bpf > $@

swiftctl: swiftctl.c swiftctl_args.c swiftctl_args.h swift.skel.h swift_types.h
	$(CC) $(CFLAGS) -I. -o $@ swiftctl.c swiftctl_args.c $(LIBS)

# Loads into the kernel (runs the verifier) without registering, then cleans up.
check: swift.bpf.o
	sudo rm -rf /sys/fs/bpf/swift_check
	sudo $(BPFTOOL) prog loadall swift.bpf.o /sys/fs/bpf/swift_check && echo "verifier: OK"
	sudo rm -rf /sys/fs/bpf/swift_check

clean:
	rm -f swift.bpf.o swift.skel.h swiftctl vmlinux.h
