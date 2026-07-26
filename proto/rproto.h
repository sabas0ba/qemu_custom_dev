/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rproto.h - renderer protocol v0 (transport-independent)
 *
 * Wire format definitions shared by the host daemon and guest clients.
 * All integers are little-endian on the wire. Encode/decode goes through
 * explicit byte access, never through struct casts, so the definitions
 * are safe regardless of compiler padding or host endianness.
 */
#ifndef RPROTO_H
#define RPROTO_H

#include <stddef.h>
#include <stdint.h>

#define RPROTO_MAGIC       0x30524456u /* "VDR0" in little-endian byte order */
#define RPROTO_VER_MAJOR   0u
#define RPROTO_VER_MINOR   1u

/* Fixed message header: type(u32) seq(u32) payload_len(u32) */
#define RPROTO_HDR_SIZE    12u
#define RPROTO_MAX_PAYLOAD 4096u

/* Surface size limits (v0: single RGBA8888 surface per session) */
#define RPROTO_MAX_DIM     16384u

enum rproto_msg_type {
    RPROTO_MSG_HELLO          = 1, /* client -> server */
    RPROTO_MSG_HELLO_ACK      = 2, /* server -> client */
    RPROTO_MSG_CREATE_SURFACE = 3, /* client -> server */
    RPROTO_MSG_CLEAR          = 4, /* client -> server */
    RPROTO_MSG_FILL_RECT      = 5, /* client -> server */
    RPROTO_MSG_PRESENT        = 6, /* client -> server */
    RPROTO_MSG_GOODBYE        = 7, /* client -> server */
    RPROTO_MSG_STATUS         = 8, /* server -> client */
};

enum rproto_status {
    RPROTO_ST_OK           = 0,
    RPROTO_ST_ERR_PROTO    = 1, /* malformed message / bad magic / version */
    RPROTO_ST_ERR_STATE    = 2, /* message not valid in current session state */
    RPROTO_ST_ERR_ARG      = 3, /* argument out of range */
    RPROTO_ST_ERR_INTERNAL = 4,
};

struct rproto_hdr {
    uint32_t type;
    uint32_t seq;
    uint32_t payload_len;
};

struct rproto_hello {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
};

struct rproto_hello_ack {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
    uint32_t max_payload;
};

struct rproto_create_surface {
    uint32_t width;
    uint32_t height;
};

/* Colors are 0xRRGGBBAA packed into a u32. */
struct rproto_clear {
    uint32_t rgba;
};

struct rproto_fill_rect {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t rgba;
};

struct rproto_present {
    uint32_t frame_id;
};

struct rproto_status_msg {
    uint32_t status;  /* enum rproto_status */
    uint32_t seq_ref; /* seq of the request this responds to */
};

/* Exact payload sizes on the wire */
#define RPROTO_LEN_HELLO          8u
#define RPROTO_LEN_HELLO_ACK      12u
#define RPROTO_LEN_CREATE_SURFACE 8u
#define RPROTO_LEN_CLEAR          4u
#define RPROTO_LEN_FILL_RECT      20u
#define RPROTO_LEN_PRESENT        4u
#define RPROTO_LEN_GOODBYE        0u
#define RPROTO_LEN_STATUS         8u

/* Header codec */
void rproto_encode_hdr(uint8_t buf[RPROTO_HDR_SIZE], const struct rproto_hdr *h);
int rproto_decode_hdr(const uint8_t *buf, size_t len, struct rproto_hdr *h);

/*
 * Payload codecs. Encoders return the number of bytes written (always the
 * RPROTO_LEN_* constant). Decoders return 0 on success, -1 when len does not
 * match the expected size for the message.
 */
uint32_t rproto_enc_hello(uint8_t *buf, const struct rproto_hello *m);
int rproto_dec_hello(const uint8_t *buf, uint32_t len, struct rproto_hello *m);

uint32_t rproto_enc_hello_ack(uint8_t *buf, const struct rproto_hello_ack *m);
int rproto_dec_hello_ack(const uint8_t *buf, uint32_t len, struct rproto_hello_ack *m);

uint32_t rproto_enc_create_surface(uint8_t *buf, const struct rproto_create_surface *m);
int rproto_dec_create_surface(const uint8_t *buf, uint32_t len, struct rproto_create_surface *m);

uint32_t rproto_enc_clear(uint8_t *buf, const struct rproto_clear *m);
int rproto_dec_clear(const uint8_t *buf, uint32_t len, struct rproto_clear *m);

uint32_t rproto_enc_fill_rect(uint8_t *buf, const struct rproto_fill_rect *m);
int rproto_dec_fill_rect(const uint8_t *buf, uint32_t len, struct rproto_fill_rect *m);

uint32_t rproto_enc_present(uint8_t *buf, const struct rproto_present *m);
int rproto_dec_present(const uint8_t *buf, uint32_t len, struct rproto_present *m);

uint32_t rproto_enc_status(uint8_t *buf, const struct rproto_status_msg *m);
int rproto_dec_status(const uint8_t *buf, uint32_t len, struct rproto_status_msg *m);

/*
 * Framed I/O over a connected stream fd (vsock, unix, tcp, ...).
 *
 * rproto_send: writes header + payload. Returns 0 on success, -1 on error.
 * rproto_recv: reads one message into *hdr and payload (capacity cap).
 *   Returns 0 on success, 1 on orderly EOF before a header byte was read,
 *   -1 on error (including payload_len > cap or > RPROTO_MAX_PAYLOAD).
 */
int rproto_send(int fd, uint32_t type, uint32_t seq,
                const uint8_t *payload, uint32_t payload_len);
int rproto_recv(int fd, struct rproto_hdr *hdr, uint8_t *payload, uint32_t cap);

#endif /* RPROTO_H */
