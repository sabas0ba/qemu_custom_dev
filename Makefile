# Build for host-side tools and guest userspace clients.
# Dependencies: a C11 toolchain and GNU make only.
CC       ?= cc
CFLAGS   ?= -O2 -g
# "override" keeps these even when CFLAGS is given on the command line
# (e.g. make CFLAGS="-O2 -g -Werror" in CI).
override CFLAGS += -std=c11 -Wall -Wextra
CPPFLAGS += -Iproto -D_GNU_SOURCE

BUILD := build

PROTO_SRCS := proto/rproto.c proto/rproto_io.c

BINS := $(BUILD)/renderd $(BUILD)/demo $(BUILD)/test_proto $(BUILD)/ppm_check

.PHONY: all test clean

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/renderd: host/renderd.c $(PROTO_SRCS) proto/rproto.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ host/renderd.c $(PROTO_SRCS)

$(BUILD)/demo: guest/user/demo.c guest/user/render_client.c $(PROTO_SRCS) \
		proto/rproto.h guest/user/render_client.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iguest/user -o $@ \
		guest/user/demo.c guest/user/render_client.c $(PROTO_SRCS)

$(BUILD)/test_proto: tests/test_proto.c $(PROTO_SRCS) proto/rproto.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_proto.c $(PROTO_SRCS)

$(BUILD)/ppm_check: tests/ppm_check.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/ppm_check.c

test: all
	$(BUILD)/test_proto
	tests/e2e.sh

clean:
	rm -rf $(BUILD)
