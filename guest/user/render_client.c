#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "rproto.h"
#include "render_client.h"

int rc_connect_unix(struct render_client *rc, const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    rc->fd = fd;
    rc->next_seq = 1;
    return 0;
}

int rc_connect_vsock(struct render_client *rc, uint32_t cid, uint32_t port)
{
    struct sockaddr_vm sa = {
        .svm_family = AF_VSOCK,
        .svm_cid = cid,
        .svm_port = port,
    };
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    rc->fd = fd;
    rc->next_seq = 1;
    return 0;
}

/* Send one request and wait for the matching STATUS reply. */
static int rc_call(struct render_client *rc, uint32_t type,
                   const uint8_t *payload, uint32_t payload_len)
{
    uint8_t rbuf[RPROTO_MAX_PAYLOAD];
    struct rproto_hdr hdr;
    struct rproto_status_msg st;
    uint32_t seq = rc->next_seq++;

    if (rproto_send(rc->fd, type, seq, payload, payload_len) < 0)
        return -1;
    if (rproto_recv(rc->fd, &hdr, rbuf, sizeof(rbuf)) != 0)
        return -1;
    if (hdr.type != RPROTO_MSG_STATUS ||
        rproto_dec_status(rbuf, hdr.payload_len, &st) < 0)
        return -1;
    if (st.seq_ref != seq || st.status != RPROTO_ST_OK) {
        fprintf(stderr, "render_client: request type=%u failed, status=%u\n",
                type, st.status);
        return -1;
    }
    return 0;
}

int rc_hello(struct render_client *rc)
{
    uint8_t buf[RPROTO_MAX_PAYLOAD];
    struct rproto_hdr hdr;
    struct rproto_hello hello = {
        .magic = RPROTO_MAGIC,
        .ver_major = RPROTO_VER_MAJOR,
        .ver_minor = RPROTO_VER_MINOR,
    };
    struct rproto_hello_ack ack;
    uint32_t len = rproto_enc_hello(buf, &hello);
    uint32_t seq = rc->next_seq++;

    if (rproto_send(rc->fd, RPROTO_MSG_HELLO, seq, buf, len) < 0)
        return -1;
    if (rproto_recv(rc->fd, &hdr, buf, sizeof(buf)) != 0)
        return -1;
    if (hdr.type != RPROTO_MSG_HELLO_ACK ||
        rproto_dec_hello_ack(buf, hdr.payload_len, &ack) < 0 ||
        ack.magic != RPROTO_MAGIC || ack.ver_major != RPROTO_VER_MAJOR)
        return -1;
    return 0;
}

int rc_create_surface(struct render_client *rc, uint32_t width, uint32_t height)
{
    uint8_t buf[RPROTO_LEN_CREATE_SURFACE];
    struct rproto_create_surface m = { .width = width, .height = height };

    return rc_call(rc, RPROTO_MSG_CREATE_SURFACE, buf,
                   rproto_enc_create_surface(buf, &m));
}

int rc_clear(struct render_client *rc, uint32_t rgba)
{
    uint8_t buf[RPROTO_LEN_CLEAR];
    struct rproto_clear m = { .rgba = rgba };

    return rc_call(rc, RPROTO_MSG_CLEAR, buf, rproto_enc_clear(buf, &m));
}

int rc_fill_rect(struct render_client *rc, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t rgba)
{
    uint8_t buf[RPROTO_LEN_FILL_RECT];
    struct rproto_fill_rect m = { .x = x, .y = y, .w = w, .h = h, .rgba = rgba };

    return rc_call(rc, RPROTO_MSG_FILL_RECT, buf, rproto_enc_fill_rect(buf, &m));
}

int rc_present(struct render_client *rc, uint32_t frame_id)
{
    uint8_t buf[RPROTO_LEN_PRESENT];
    struct rproto_present m = { .frame_id = frame_id };

    return rc_call(rc, RPROTO_MSG_PRESENT, buf, rproto_enc_present(buf, &m));
}

int rc_goodbye(struct render_client *rc)
{
    return rc_call(rc, RPROTO_MSG_GOODBYE, NULL, 0);
}

void rc_close(struct render_client *rc)
{
    if (rc->fd >= 0) {
        close(rc->fd);
        rc->fd = -1;
    }
}
