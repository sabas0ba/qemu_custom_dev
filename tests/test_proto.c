/* SPDX-License-Identifier: GPL-2.0-only */
/* Unit tests for the protocol codec (proto/rproto.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rproto.h"

static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void test_hdr_roundtrip(void)
{
    uint8_t buf[RPROTO_HDR_SIZE];
    struct rproto_hdr in = { .type = RPROTO_MSG_FILL_RECT, .seq = 0xdeadbeef,
                             .payload_len = RPROTO_LEN_FILL_RECT };
    struct rproto_hdr out;

    rproto_encode_hdr(buf, &in);
    CHECK(rproto_decode_hdr(buf, sizeof(buf), &out) == 0);
    CHECK(out.type == in.type);
    CHECK(out.seq == in.seq);
    CHECK(out.payload_len == in.payload_len);
}

static void test_hdr_wire_layout(void)
{
    /* Pin the little-endian byte layout so both sides agree forever. */
    uint8_t buf[RPROTO_HDR_SIZE];
    struct rproto_hdr h = { .type = 0x04030201, .seq = 0x08070605,
                            .payload_len = 0x0c0b0a09 };
    static const uint8_t expect[RPROTO_HDR_SIZE] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    };

    rproto_encode_hdr(buf, &h);
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
}

static void test_hdr_errors(void)
{
    uint8_t buf[RPROTO_HDR_SIZE];
    struct rproto_hdr h = { .type = 1, .seq = 1,
                            .payload_len = RPROTO_MAX_PAYLOAD + 1 };
    struct rproto_hdr out;

    CHECK(rproto_decode_hdr(buf, RPROTO_HDR_SIZE - 1, &out) == -1);
    rproto_encode_hdr(buf, &h);
    CHECK(rproto_decode_hdr(buf, sizeof(buf), &out) == -1);
}

static void test_hello_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_HELLO];
    struct rproto_hello in = { .magic = RPROTO_MAGIC,
                               .ver_major = 3, .ver_minor = 7 };
    struct rproto_hello out;

    CHECK(rproto_enc_hello(buf, &in) == RPROTO_LEN_HELLO);
    CHECK(rproto_dec_hello(buf, RPROTO_LEN_HELLO, &out) == 0);
    CHECK(out.magic == in.magic);
    CHECK(out.ver_major == in.ver_major);
    CHECK(out.ver_minor == in.ver_minor);
    CHECK(rproto_dec_hello(buf, RPROTO_LEN_HELLO - 1, &out) == -1);
    CHECK(rproto_dec_hello(buf, RPROTO_LEN_HELLO + 1, &out) == -1);
}

static void test_hello_ack_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_HELLO_ACK];
    struct rproto_hello_ack in = { .magic = RPROTO_MAGIC, .ver_major = 0,
                                   .ver_minor = 1, .max_payload = 4096 };
    struct rproto_hello_ack out;

    CHECK(rproto_enc_hello_ack(buf, &in) == RPROTO_LEN_HELLO_ACK);
    CHECK(rproto_dec_hello_ack(buf, RPROTO_LEN_HELLO_ACK, &out) == 0);
    CHECK(out.magic == in.magic);
    CHECK(out.ver_major == in.ver_major);
    CHECK(out.ver_minor == in.ver_minor);
    CHECK(out.max_payload == in.max_payload);
    CHECK(rproto_dec_hello_ack(buf, 0, &out) == -1);
}

static void test_create_surface_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_CREATE_SURFACE];
    struct rproto_create_surface in = { .width = 320, .height = 240 };
    struct rproto_create_surface out;

    CHECK(rproto_enc_create_surface(buf, &in) == RPROTO_LEN_CREATE_SURFACE);
    CHECK(rproto_dec_create_surface(buf, RPROTO_LEN_CREATE_SURFACE, &out) == 0);
    CHECK(out.width == 320 && out.height == 240);
    CHECK(rproto_dec_create_surface(buf, 4, &out) == -1);
}

static void test_clear_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_CLEAR];
    struct rproto_clear in = { .rgba = 0x102030ff };
    struct rproto_clear out;

    CHECK(rproto_enc_clear(buf, &in) == RPROTO_LEN_CLEAR);
    CHECK(rproto_dec_clear(buf, RPROTO_LEN_CLEAR, &out) == 0);
    CHECK(out.rgba == 0x102030ff);
    CHECK(rproto_dec_clear(buf, 0, &out) == -1);
}

static void test_fill_rect_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_FILL_RECT];
    struct rproto_fill_rect in = { .x = 1, .y = 2, .w = 3, .h = 4,
                                   .rgba = 0xaabbccdd };
    struct rproto_fill_rect out;

    CHECK(rproto_enc_fill_rect(buf, &in) == RPROTO_LEN_FILL_RECT);
    CHECK(rproto_dec_fill_rect(buf, RPROTO_LEN_FILL_RECT, &out) == 0);
    CHECK(out.x == 1 && out.y == 2 && out.w == 3 && out.h == 4);
    CHECK(out.rgba == 0xaabbccdd);
    CHECK(rproto_dec_fill_rect(buf, RPROTO_LEN_FILL_RECT - 4, &out) == -1);
}

static void test_present_status_roundtrip(void)
{
    uint8_t buf[RPROTO_LEN_STATUS];
    struct rproto_present pin = { .frame_id = 42 };
    struct rproto_present pout;
    struct rproto_status_msg sin = { .status = RPROTO_ST_ERR_ARG,
                                     .seq_ref = 99 };
    struct rproto_status_msg sout;

    CHECK(rproto_enc_present(buf, &pin) == RPROTO_LEN_PRESENT);
    CHECK(rproto_dec_present(buf, RPROTO_LEN_PRESENT, &pout) == 0);
    CHECK(pout.frame_id == 42);

    CHECK(rproto_enc_status(buf, &sin) == RPROTO_LEN_STATUS);
    CHECK(rproto_dec_status(buf, RPROTO_LEN_STATUS, &sout) == 0);
    CHECK(sout.status == RPROTO_ST_ERR_ARG && sout.seq_ref == 99);
}

int main(void)
{
    test_hdr_roundtrip();
    test_hdr_wire_layout();
    test_hdr_errors();
    test_hello_roundtrip();
    test_hello_ack_roundtrip();
    test_create_surface_roundtrip();
    test_clear_roundtrip();
    test_fill_rect_roundtrip();
    test_present_status_roundtrip();

    if (g_failures) {
        fprintf(stderr, "test_proto: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_proto: all tests passed\n");
    return 0;
}
