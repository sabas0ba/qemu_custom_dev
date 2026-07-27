# SPDX-License-Identifier: GPL-2.0-only
# Build for host-side tools and guest userspace clients.
# Dependencies: a C11 toolchain and GNU make only.
CC       ?= cc
CFLAGS   ?= -O2 -g
# "override" keeps these even when CFLAGS is given on the command line
# (e.g. make CFLAGS="-O2 -g -Werror" in CI).
override CFLAGS += -std=c11 -Wall -Wextra
CPPFLAGS += -Iproto -D_GNU_SOURCE

BUILD := build

PROTO_SRCS := proto/rproto.c proto/rproto_io.c proto/rproto_shm.c
PROTO_HDRS := proto/rproto.h proto/rproto_shm.h

BINS := $(BUILD)/renderd $(BUILD)/ivshmemd $(BUILD)/demo $(BUILD)/bench \
	$(BUILD)/test_proto $(BUILD)/test_shm $(BUILD)/ivshmem_peer \
	$(BUILD)/ppm_check $(BUILD)/ppm2png

RENDERD_SRCS := host/renderd.c host/ivshmem.c host/vhost_user.c $(PROTO_SRCS)
CLIENT_SRCS  := guest/user/render_client.c $(PROTO_SRCS)
DEMO_SRCS    := guest/user/demo.c $(CLIENT_SRCS)
BENCH_SRCS   := guest/user/bench.c $(CLIENT_SRCS)
# The guest userspace includes the driver's uapi header straight from the
# module sources, so the two can never drift apart.
CLIENT_CPPFLAGS := -Iguest/user -Iguest/kmod

.PHONY: all test clean

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/renderd: $(RENDERD_SRCS) $(PROTO_HDRS) host/ivshmem.h \
		host/vhost_user.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ihost -o $@ $(RENDERD_SRCS)

$(BUILD)/ivshmemd: host/ivshmemd.c host/ivshmem.c host/ivshmem.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ihost -o $@ host/ivshmemd.c host/ivshmem.c

$(BUILD)/demo: $(DEMO_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_CPPFLAGS) -o $@ $(DEMO_SRCS)

$(BUILD)/bench: $(BENCH_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_CPPFLAGS) -o $@ $(BENCH_SRCS)

# Statically linked builds for running inside a guest without a toolchain
# (shared into the VM over 9p by tests/vm-e2e.sh).
$(BUILD)/demo-static: $(DEMO_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_CPPFLAGS) -static -o $@ $(DEMO_SRCS)

$(BUILD)/bench-static: $(BENCH_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_CPPFLAGS) -static -o $@ $(BENCH_SRCS)

$(BUILD)/test_proto: tests/test_proto.c $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_proto.c $(PROTO_SRCS)

$(BUILD)/test_shm: tests/test_shm.c $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_shm.c $(PROTO_SRCS)

$(BUILD)/ivshmem_peer: tests/ivshmem_peer.c host/ivshmem.c $(CLIENT_SRCS) \
		$(PROTO_HDRS) host/ivshmem.h guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_CPPFLAGS) -Ihost -o $@ \
		tests/ivshmem_peer.c host/ivshmem.c $(CLIENT_SRCS)

$(BUILD)/ppm_check: tests/ppm_check.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/ppm_check.c

$(BUILD)/ppm2png: tools/ppm2png.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/ppm2png.c

test: all
	$(BUILD)/test_proto
	$(BUILD)/test_shm
	tests/e2e.sh

clean:
	rm -rf $(BUILD)
