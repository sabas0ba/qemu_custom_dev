/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rproto_io.c - framed message I/O over a connected stream fd.
 *
 * Works on any SOCK_STREAM-like fd (AF_VSOCK, AF_UNIX, pipes for tests).
 * This is deliberately the only place that touches read()/write() so the
 * protocol layer stays transport-independent.
 */
#include <errno.h>
#include <unistd.h>

#include "rproto.h"

static int write_full(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Returns 1 if fully read, 0 on EOF at the first byte, -1 on error/short EOF. */
static int read_full(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return got == 0 ? 0 : -1;
        got += (size_t)n;
    }
    return 1;
}

int rproto_send(int fd, uint32_t type, uint32_t seq,
                const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hdr[RPROTO_HDR_SIZE];
    struct rproto_hdr h = { .type = type, .seq = seq, .payload_len = payload_len };

    if (payload_len > RPROTO_MAX_PAYLOAD)
        return -1;
    rproto_encode_hdr(hdr, &h);
    if (write_full(fd, hdr, sizeof(hdr)) < 0)
        return -1;
    if (payload_len > 0 && write_full(fd, payload, payload_len) < 0)
        return -1;
    return 0;
}

int rproto_recv(int fd, struct rproto_hdr *hdr, uint8_t *payload, uint32_t cap)
{
    uint8_t hbuf[RPROTO_HDR_SIZE];
    int r = read_full(fd, hbuf, sizeof(hbuf));

    if (r == 0)
        return 1; /* orderly EOF */
    if (r < 0)
        return -1;
    if (rproto_decode_hdr(hbuf, sizeof(hbuf), hdr) < 0)
        return -1;
    if (hdr->payload_len > cap)
        return -1;
    if (hdr->payload_len > 0 &&
        read_full(fd, payload, hdr->payload_len) != 1)
        return -1;
    return 0;
}
