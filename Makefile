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

BINS := $(BUILD)/renderd $(BUILD)/demo $(BUILD)/test_proto $(BUILD)/test_shm \
	$(BUILD)/ppm_check

DEMO_SRCS := guest/user/demo.c guest/user/render_client.c $(PROTO_SRCS)

.PHONY: all test clean

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/renderd: host/renderd.c $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ host/renderd.c $(PROTO_SRCS)

$(BUILD)/demo: $(DEMO_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iguest/user -o $@ $(DEMO_SRCS)

# Statically linked demo for running inside a guest without a toolchain
# (shared into the VM over 9p by tests/vm-e2e.sh).
$(BUILD)/demo-static: $(DEMO_SRCS) $(PROTO_HDRS) guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -static -Iguest/user -o $@ $(DEMO_SRCS)

$(BUILD)/test_proto: tests/test_proto.c $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_proto.c $(PROTO_SRCS)

$(BUILD)/test_shm: tests/test_shm.c $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_shm.c $(PROTO_SRCS)

$(BUILD)/ppm_check: tests/ppm_check.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/ppm_check.c

test: all
	$(BUILD)/test_proto
	$(BUILD)/test_shm
	tests/e2e.sh

clean:
	rm -rf $(BUILD)
