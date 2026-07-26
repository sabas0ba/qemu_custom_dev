/* SPDX-License-Identifier: GPL-2.0-only */
/* Unit tests for the shared-memory ring transport (proto/rproto_shm.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rproto_shm.h"

static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void test_init_attach(void)
{
    void *mem = calloc(1, RSHM_DEFAULT_SIZE);
    struct rshm host, guest;

    CHECK(rshm_init(&host, mem, RSHM_DEFAULT_SIZE, -1) == 0);
    CHECK(rshm_attach(&guest, mem, RSHM_DEFAULT_SIZE, 0) == 0);
    CHECK(guest.hdr->total_size == RSHM_DEFAULT_SIZE);
    CHECK(guest.g2h.size == RSHM_RING_SIZE);
    CHECK(guest.hdr->fb_size > 0);

    /* init on a too-small region must fail */
    struct rshm tiny;

    CHECK(rshm_init(&tiny, mem, 1024, -1) == -1);
    free(mem);
}

static void test_attach_timeout(void)
{
    void *mem = calloc(1, RSHM_DEFAULT_SIZE);
    struct rshm s;

    /* nothing published: immediate check and short timeout must fail */
    CHECK(rshm_attach(&s, mem, RSHM_DEFAULT_SIZE, 0) == -1);
    CHECK(rshm_attach(&s, mem, RSHM_DEFAULT_SIZE, 1) == -1);
    free(mem);
}

static void test_roundtrip(void)
{
    void *mem = calloc(1, RSHM_DEFAULT_SIZE);
    struct rshm host, guest;
    struct rproto_hdr hdr;
    uint8_t payload[RPROTO_MAX_PAYLOAD];
    uint8_t out[RPROTO_MAX_PAYLOAD];

    CHECK(rshm_init(&host, mem, RSHM_DEFAULT_SIZE, -1) == 0);
    CHECK(rshm_attach(&guest, mem, RSHM_DEFAULT_SIZE, 0) == 0);

    /* empty ring: recv would block */
    CHECK(rshm_msg_try_recv(&host.g2h, &hdr, out, sizeof(out)) == 1);

    for (uint32_t i = 0; i < 64; i++)
        payload[i] = (uint8_t)(i * 7);
    /* guest -> host */
    CHECK(rshm_msg_try_send(&guest.g2h, RPROTO_MSG_FILL_RECT, 42,
                            payload, 64) == 0);
    CHECK(rshm_msg_try_recv(&host.g2h, &hdr, out, sizeof(out)) == 0);
    CHECK(hdr.type == RPROTO_MSG_FILL_RECT);
    CHECK(hdr.seq == 42);
    CHECK(hdr.payload_len == 64);
    CHECK(memcmp(out, payload, 64) == 0);

    /* host -> guest, zero-length payload */
    CHECK(rshm_msg_try_send(&host.h2g, RPROTO_MSG_STATUS, 7, NULL, 0) == 0);
    CHECK(rshm_msg_try_recv(&guest.h2g, &hdr, out, sizeof(out)) == 0);
    CHECK(hdr.type == RPROTO_MSG_STATUS);
    CHECK(hdr.payload_len == 0);

    /* oversized payload is rejected outright */
    CHECK(rshm_msg_try_send(&guest.g2h, 1, 1, payload,
                            RPROTO_MAX_PAYLOAD + 1) == -1);
    free(mem);
}

static void test_fill_and_wraparound(void)
{
    void *mem = calloc(1, RSHM_DEFAULT_SIZE);
    struct rshm host, guest;
    struct rproto_hdr hdr;
    uint8_t payload[RPROTO_MAX_PAYLOAD];
    uint8_t out[RPROTO_MAX_PAYLOAD];
    uint32_t sent = 0, received = 0;

    CHECK(rshm_init(&host, mem, RSHM_DEFAULT_SIZE, -1) == 0);
    CHECK(rshm_attach(&guest, mem, RSHM_DEFAULT_SIZE, 0) == 0);

    /* fill the ring until it reports no space */
    memset(payload, 0xa5, sizeof(payload));
    for (;;) {
        int rc = rshm_msg_try_send(&guest.g2h, 5, sent, payload, 1000);

        CHECK(rc == 0 || rc == 1);
        if (rc != 0)
            break;
        sent++;
    }
    CHECK(sent > 100); /* 256 KiB ring / ~1 KiB messages */

    /* drain everything and verify sequence numbers */
    for (;;) {
        int rc = rshm_msg_try_recv(&host.g2h, &hdr, out, sizeof(out));

        CHECK(rc == 0 || rc == 1);
        if (rc != 0)
            break;
        CHECK(hdr.seq == received);
        CHECK(hdr.payload_len == 1000);
        received++;
    }
    CHECK(received == sent);

    /* push many odd-sized messages so positions wrap the ring repeatedly
     * and free-running indices keep advancing; verify data integrity */
    for (uint32_t i = 0; i < 5000; i++) {
        uint32_t len = 1 + (i * 37) % 1500;

        for (uint32_t j = 0; j < len; j++)
            payload[j] = (uint8_t)(i + j);
        CHECK(rshm_msg_try_send(&guest.g2h, 5, i, payload, len) == 0);
        CHECK(rshm_msg_try_recv(&host.g2h, &hdr, out, sizeof(out)) == 0);
        CHECK(hdr.seq == i);
        CHECK(hdr.payload_len == len);
        CHECK(memcmp(out, payload, len) == 0);
    }

    /* blocking variants: timeout on empty, success when data arrives */
    CHECK(rshm_msg_recv(&host.g2h, &hdr, out, sizeof(out), 1) == 1);
    CHECK(rshm_msg_send(&guest.g2h, 6, 1, NULL, 0, 10) == 0);
    CHECK(rshm_msg_recv(&host.g2h, &hdr, out, sizeof(out), 10) == 0);
    CHECK(hdr.type == 6);
    free(mem);
}

static void test_reset(void)
{
    void *mem = calloc(1, RSHM_DEFAULT_SIZE);
    struct rshm host, guest;
    struct rproto_hdr hdr;
    uint8_t out[RPROTO_MAX_PAYLOAD];

    CHECK(rshm_init(&host, mem, RSHM_DEFAULT_SIZE, -1) == 0);
    CHECK(rshm_attach(&guest, mem, RSHM_DEFAULT_SIZE, 0) == 0);
    CHECK(rshm_msg_try_send(&guest.g2h, 3, 1, NULL, 0) == 0);
    rshm_reset_rings(&host);
    CHECK(rshm_msg_try_recv(&host.g2h, &hdr, out, sizeof(out)) == 1);
    free(mem);
}

int main(void)
{
    test_init_attach();
    test_attach_timeout();
    test_roundtrip();
    test_fill_and_wraparound();
    test_reset();

    if (g_failures) {
        fprintf(stderr, "test_shm: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_shm: all tests passed\n");
    return 0;
}
