/* SPDX-License-Identifier: GPL-2.0-only */
#include "rproto.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

/* Signed coordinates travel two's-complement in a u32 slot. */
static void puti32(uint8_t *p, int32_t v)
{
    put32(p, (uint32_t)v);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t geti32(const uint8_t *p);

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void rproto_encode_hdr(uint8_t buf[RPROTO_HDR_SIZE], const struct rproto_hdr *h)
{
    put32(buf + 0, h->type);
    put32(buf + 4, h->seq);
    put32(buf + 8, h->payload_len);
}

int rproto_decode_hdr(const uint8_t *buf, size_t len, struct rproto_hdr *h)
{
    if (len < RPROTO_HDR_SIZE)
        return -1;
    h->type = get32(buf + 0);
    h->seq = get32(buf + 4);
    h->payload_len = get32(buf + 8);
    if (h->payload_len > RPROTO_MAX_PAYLOAD)
        return -1;
    return 0;
}

uint32_t rproto_enc_hello(uint8_t *buf, const struct rproto_hello *m)
{
    put32(buf + 0, m->magic);
    put16(buf + 4, m->ver_major);
    put16(buf + 6, m->ver_minor);
    return RPROTO_LEN_HELLO;
}

int rproto_dec_hello(const uint8_t *buf, uint32_t len, struct rproto_hello *m)
{
    if (len != RPROTO_LEN_HELLO)
        return -1;
    m->magic = get32(buf + 0);
    m->ver_major = get16(buf + 4);
    m->ver_minor = get16(buf + 6);
    return 0;
}

uint32_t rproto_enc_hello_ack(uint8_t *buf, const struct rproto_hello_ack *m)
{
    put32(buf + 0, m->magic);
    put16(buf + 4, m->ver_major);
    put16(buf + 6, m->ver_minor);
    put32(buf + 8, m->max_payload);
    return RPROTO_LEN_HELLO_ACK;
}

int rproto_dec_hello_ack(const uint8_t *buf, uint32_t len, struct rproto_hello_ack *m)
{
    if (len != RPROTO_LEN_HELLO_ACK)
        return -1;
    m->magic = get32(buf + 0);
    m->ver_major = get16(buf + 4);
    m->ver_minor = get16(buf + 6);
    m->max_payload = get32(buf + 8);
    return 0;
}

uint32_t rproto_enc_create_surface(uint8_t *buf, const struct rproto_create_surface *m)
{
    put32(buf + 0, m->width);
    put32(buf + 4, m->height);
    return RPROTO_LEN_CREATE_SURFACE;
}

int rproto_dec_create_surface(const uint8_t *buf, uint32_t len,
                              struct rproto_create_surface *m)
{
    if (len != RPROTO_LEN_CREATE_SURFACE)
        return -1;
    m->width = get32(buf + 0);
    m->height = get32(buf + 4);
    return 0;
}

uint32_t rproto_enc_clear(uint8_t *buf, const struct rproto_clear *m)
{
    put32(buf, m->rgba);
    return RPROTO_LEN_CLEAR;
}

int rproto_dec_clear(const uint8_t *buf, uint32_t len, struct rproto_clear *m)
{
    if (len != RPROTO_LEN_CLEAR)
        return -1;
    m->rgba = get32(buf);
    return 0;
}

uint32_t rproto_enc_fill_rect(uint8_t *buf, const struct rproto_fill_rect *m)
{
    put32(buf + 0, m->x);
    put32(buf + 4, m->y);
    put32(buf + 8, m->w);
    put32(buf + 12, m->h);
    put32(buf + 16, m->rgba);
    return RPROTO_LEN_FILL_RECT;
}

int rproto_dec_fill_rect(const uint8_t *buf, uint32_t len, struct rproto_fill_rect *m)
{
    if (len != RPROTO_LEN_FILL_RECT)
        return -1;
    m->x = get32(buf + 0);
    m->y = get32(buf + 4);
    m->w = get32(buf + 8);
    m->h = get32(buf + 12);
    m->rgba = get32(buf + 16);
    return 0;
}

uint32_t rproto_enc_present(uint8_t *buf, const struct rproto_present *m)
{
    put32(buf, m->frame_id);
    return RPROTO_LEN_PRESENT;
}

int rproto_dec_present(const uint8_t *buf, uint32_t len, struct rproto_present *m)
{
    if (len != RPROTO_LEN_PRESENT)
        return -1;
    m->frame_id = get32(buf);
    return 0;
}

uint32_t rproto_enc_status(uint8_t *buf, const struct rproto_status_msg *m)
{
    put32(buf + 0, m->status);
    put32(buf + 4, m->seq_ref);
    return RPROTO_LEN_STATUS;
}

int rproto_dec_status(const uint8_t *buf, uint32_t len, struct rproto_status_msg *m)
{
    if (len != RPROTO_LEN_STATUS)
        return -1;
    m->status = get32(buf + 0);
    m->seq_ref = get32(buf + 4);
    return 0;
}

uint32_t rproto_enc_blit(uint8_t *buf, const struct rproto_blit *m)
{
    put32(buf + 0, m->src_off);
    put32(buf + 4, m->stride);
    put32(buf + 8, m->x);
    put32(buf + 12, m->y);
    put32(buf + 16, m->w);
    put32(buf + 20, m->h);
    return RPROTO_LEN_BLIT;
}

int rproto_dec_blit(const uint8_t *buf, uint32_t len, struct rproto_blit *m)
{
    if (len != RPROTO_LEN_BLIT)
        return -1;
    m->src_off = get32(buf + 0);
    m->stride = get32(buf + 4);
    m->x = get32(buf + 8);
    m->y = get32(buf + 12);
    m->w = get32(buf + 16);
    m->h = get32(buf + 20);
    return 0;
}

static int32_t geti32(const uint8_t *p)
{
    return (int32_t)get32(p);
}

uint32_t rproto_enc_set_blend(uint8_t *buf, const struct rproto_set_blend *m)
{
    put32(buf, m->mode);
    return RPROTO_LEN_SET_BLEND;
}

int rproto_dec_set_blend(const uint8_t *buf, uint32_t len,
                         struct rproto_set_blend *m)
{
    if (len != RPROTO_LEN_SET_BLEND)
        return -1;
    m->mode = get32(buf);
    return 0;
}

uint32_t rproto_enc_draw_line(uint8_t *buf, const struct rproto_draw_line *m)
{
    puti32(buf + 0, m->x0);
    puti32(buf + 4, m->y0);
    puti32(buf + 8, m->x1);
    puti32(buf + 12, m->y1);
    put32(buf + 16, m->rgba);
    return RPROTO_LEN_DRAW_LINE;
}

int rproto_dec_draw_line(const uint8_t *buf, uint32_t len,
                         struct rproto_draw_line *m)
{
    if (len != RPROTO_LEN_DRAW_LINE)
        return -1;
    m->x0 = geti32(buf + 0);
    m->y0 = geti32(buf + 4);
    m->x1 = geti32(buf + 8);
    m->y1 = geti32(buf + 12);
    m->rgba = get32(buf + 16);
    return 0;
}

uint32_t rproto_enc_draw_triangle(uint8_t *buf,
                                  const struct rproto_draw_triangle *m)
{
    puti32(buf + 0, m->x0);
    puti32(buf + 4, m->y0);
    puti32(buf + 8, m->x1);
    puti32(buf + 12, m->y1);
    puti32(buf + 16, m->x2);
    puti32(buf + 20, m->y2);
    put32(buf + 24, m->rgba);
    return RPROTO_LEN_DRAW_TRIANGLE;
}

int rproto_dec_draw_triangle(const uint8_t *buf, uint32_t len,
                             struct rproto_draw_triangle *m)
{
    if (len != RPROTO_LEN_DRAW_TRIANGLE)
        return -1;
    m->x0 = geti32(buf + 0);
    m->y0 = geti32(buf + 4);
    m->x1 = geti32(buf + 8);
    m->y1 = geti32(buf + 12);
    m->x2 = geti32(buf + 16);
    m->y2 = geti32(buf + 20);
    m->rgba = get32(buf + 24);
    return 0;
}
