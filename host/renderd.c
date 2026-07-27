/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * renderd - host-side renderer daemon (Phase 1).
 *
 * Listens on AF_VSOCK (for a QEMU guest) or AF_UNIX (for local testing),
 * speaks renderer protocol v0, and renders into an in-memory RGBA8888
 * framebuffer. Each PRESENT dumps the frame as a binary PPM (P6) file
 * into the output directory.
 *
 * v0 scope: one client at a time, one surface per session, no timeouts.
 *
 * Usage:
 *   renderd --unix PATH --out DIR
 *   renderd --vsock PORT --out DIR
 *   renderd --tcp PORT --out DIR
 *   renderd --shm FILE --out DIR
 *
 * The tcp listener binds 127.0.0.1 only. It exists for development and CI
 * on hosts without /dev/vhost-vsock; a QEMU guest on user-mode networking
 * reaches it via the slirp gateway 10.0.2.2, which maps to the host
 * loopback.
 *
 * --shm is the Phase 2 ivshmem transport: FILE is created and initialized
 * here, handed to QEMU as a share=on memory-backend-file for
 * ivshmem-plain, and polled for messages (no interrupts in Phase 2).
 *
 * --ivshmem is the Phase 3 doorbell transport: we join SOCKET as a peer of
 * an ivshmem server (host/ivshmemd), which also hands the shared memory
 * and the notification eventfds to QEMU's ivshmem-doorbell device. Same
 * rings as --shm, but each side is woken by an interrupt instead of
 * spinning on the ring.
 *
 * --vhost-user is the Phase 4 transport: we are the vhost-user backend
 * behind QEMU's generic vhost-user-device, so the guest sees a real
 * virtio device and the messages ride a split vring in guest RAM rather
 * than a ring we invented. See host/vhost_user.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "ivshmem.h"
#include "rproto.h"
#include "rproto_shm.h"
#include "vhost_user.h"

/*
 * Transport-independent session I/O. recv follows rproto_recv semantics
 * (0 = got a message, 1 = peer gone / would never complete, -1 = error);
 * send returns 0 or -1.
 */
struct rio {
    int (*recv)(void *ctx, struct rproto_hdr *hdr, uint8_t *payload,
                uint32_t cap);
    int (*send)(void *ctx, uint32_t type, uint32_t seq,
                const uint8_t *payload, uint32_t payload_len);
    void *ctx;
    /*
     * Staging area for BLIT: the part of the shared region the client
     * writes pixels into. NULL on the stream transports, which have no
     * shared memory and therefore no BLIT.
     */
    const uint8_t *staging;
    uint32_t staging_size;
};

struct session {
    struct rio *io;
    int hello_done;
    uint32_t width;
    uint32_t height;
    uint32_t blend; /* enum rproto_blend; REPLACE unless SET_BLEND says so */
    uint32_t *fb;   /* width*height pixels, 0xRRGGBBAA */
};

static const char *g_outdir = ".";

/*
 * One channel of source-over compositing, rounded to nearest:
 *   out = (s*a + d*(255-a) + 127) / 255
 * Integer and exact, so the tests can predict every pixel. See
 * docs/renderer.md.
 */
static uint32_t blend_chan(uint32_t s, uint32_t d, uint32_t a)
{
    return (s * a + d * (255u - a) + 127u) / 255u;
}

/* Combine one source pixel with the destination under the session's mode. */
static uint32_t blend_px(const struct session *s, uint32_t src, uint32_t dst)
{
    uint32_t sa;

    if (s->blend == RPROTO_BLEND_REPLACE)
        return src;
    sa = src & 0xffu;
    if (sa == 255u)
        return src;
    if (sa == 0u)
        return dst;
    return (blend_chan((src >> 24) & 0xff, (dst >> 24) & 0xff, sa) << 24) |
           (blend_chan((src >> 16) & 0xff, (dst >> 16) & 0xff, sa) << 16) |
           (blend_chan((src >> 8) & 0xff, (dst >> 8) & 0xff, sa) << 8) |
           /*
            * out alpha = sa + da*(1-sa), which is the same expression with
            * a fully opaque source: blend_chan caps it at 255, where
            * adding the terms by hand would overflow into the blue
            * channel.
            */
           blend_chan(255u, dst & 0xffu, sa);
}

/* Write one pixel, ignoring anything outside the surface. */
static void fb_put(struct session *s, int64_t x, int64_t y, uint32_t rgba)
{
    uint32_t *p;

    if (x < 0 || y < 0 || x >= (int64_t)s->width || y >= (int64_t)s->height)
        return;
    p = &s->fb[(size_t)y * s->width + (size_t)x];
    *p = blend_px(s, rgba, *p);
}

static void fb_fill_rect(struct session *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t rgba)
{
    if (x >= s->width || y >= s->height)
        return;
    if (w > s->width - x)
        w = s->width - x;
    if (h > s->height - y)
        h = s->height - y;
    for (uint32_t row = y; row < y + h; row++)
        for (uint32_t col = x; col < x + w; col++) {
            uint32_t *p = &s->fb[(size_t)row * s->width + col];

            *p = blend_px(s, rgba, *p);
        }
}

/* Bresenham, clipped per pixel rather than up front: the endpoints may sit
 * far outside the surface and the loop is cheap enough not to care. */
static void fb_line(struct session *s, const struct rproto_draw_line *m)
{
    int64_t x0 = m->x0, y0 = m->y0;
    int64_t dx = llabs((long long)m->x1 - m->x0);
    int64_t dy = -llabs((long long)m->y1 - m->y0);
    int64_t sx = m->x0 < m->x1 ? 1 : -1;
    int64_t sy = m->y0 < m->y1 ? 1 : -1;
    int64_t err = dx + dy;

    for (;;) {
        fb_put(s, x0, y0, m->rgba);
        if (x0 == m->x1 && y0 == m->y1)
            break;
        int64_t e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Edge function: twice the signed area of the triangle (a, b, p). */
static int64_t edge(int64_t ax, int64_t ay, int64_t bx, int64_t by,
                    int64_t px, int64_t py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/*
 * Filled triangle by edge functions over the bounding box, clipped to the
 * surface. Either winding is accepted: the sign of the doubled area picks
 * which way "inside" points.
 */
static void fb_triangle(struct session *s, const struct rproto_draw_triangle *m)
{
    int64_t x0 = m->x0, y0 = m->y0, x1 = m->x1, y1 = m->y1;
    int64_t x2 = m->x2, y2 = m->y2;
    int64_t area = edge(x0, y0, x1, y1, x2, y2);
    int64_t minx, maxx, miny, maxy;
    int sign;

    if (area == 0)
        return; /* degenerate: no interior to fill */
    sign = area > 0 ? 1 : -1;

    minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx > (int64_t)s->width - 1)
        maxx = (int64_t)s->width - 1;
    if (maxy > (int64_t)s->height - 1)
        maxy = (int64_t)s->height - 1;

    for (int64_t py = miny; py <= maxy; py++)
        for (int64_t px = minx; px <= maxx; px++) {
            int64_t w0 = edge(x1, y1, x2, y2, px, py) * sign;
            int64_t w1 = edge(x2, y2, x0, y0, px, py) * sign;
            int64_t w2 = edge(x0, y0, x1, y1, px, py) * sign;

            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                fb_put(s, px, py, m->rgba);
        }
}

/*
 * Copy a rectangle of R,G,B,A bytes out of the staging area into the
 * surface, clipped to it. The caller has already checked that the source
 * extent is inside the staging area.
 */
static void fb_blit(struct session *s, const struct rproto_blit *m)
{
    const uint8_t *src = s->io->staging + m->src_off;
    uint32_t w = m->w;
    uint32_t h = m->h;

    if (m->x >= s->width || m->y >= s->height)
        return;
    if (w > s->width - m->x)
        w = s->width - m->x;
    if (h > s->height - m->y)
        h = s->height - m->y;
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *sp = src + (size_t)row * m->stride;
        uint32_t *dp = s->fb + (size_t)(m->y + row) * s->width + m->x;

        for (uint32_t col = 0; col < w; col++, sp += 4) {
            uint32_t src = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) |
                           ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];

            dp[col] = blend_px(s, src, dp[col]);
        }
    }
}

static int write_ppm(const struct session *s, uint32_t frame_id)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/frame-%06u.ppm", g_outdir, frame_id);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "renderd: fopen %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
    for (size_t i = 0; i < (size_t)s->width * s->height; i++) {
        uint32_t px = s->fb[i];
        uint8_t rgb[3] = {
            (uint8_t)((px >> 24) & 0xff),
            (uint8_t)((px >> 16) & 0xff),
            (uint8_t)((px >> 8) & 0xff),
        };
        if (fwrite(rgb, 1, sizeof(rgb), f) != sizeof(rgb)) {
            fclose(f);
            return -1;
        }
    }
    if (fclose(f) != 0)
        return -1;
    fprintf(stderr, "renderd: wrote %s\n", path);
    return 0;
}

static int send_status(struct session *s, uint32_t status, uint32_t seq_ref)
{
    uint8_t payload[RPROTO_LEN_STATUS];
    struct rproto_status_msg m = { .status = status, .seq_ref = seq_ref };
    uint32_t len = rproto_enc_status(payload, &m);

    return s->io->send(s->io->ctx, RPROTO_MSG_STATUS, 0, payload, len);
}

/* Returns the status for one client message; sets *stop on GOODBYE. */
static uint32_t handle_msg(struct session *s, const struct rproto_hdr *hdr,
                           const uint8_t *payload, int *stop)
{
    switch (hdr->type) {
    case RPROTO_MSG_CREATE_SURFACE: {
        struct rproto_create_surface m;

        if (!s->hello_done)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_create_surface(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        if (m.width == 0 || m.height == 0 ||
            m.width > RPROTO_MAX_DIM || m.height > RPROTO_MAX_DIM)
            return RPROTO_ST_ERR_ARG;
        free(s->fb);
        s->fb = calloc((size_t)m.width * m.height, sizeof(uint32_t));
        if (!s->fb)
            return RPROTO_ST_ERR_INTERNAL;
        s->width = m.width;
        s->height = m.height;
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_CLEAR: {
        struct rproto_clear m;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_clear(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        /* CLEAR resets the surface, so it overwrites regardless of the
         * blend mode — compositing onto what you are clearing is not a
         * thing anyone means. */
        for (size_t i = 0; i < (size_t)s->width * s->height; i++)
            s->fb[i] = m.rgba;
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_FILL_RECT: {
        struct rproto_fill_rect m;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_fill_rect(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        fb_fill_rect(s, m.x, m.y, m.w, m.h, m.rgba);
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_BLIT: {
        struct rproto_blit m;
        uint64_t row_bytes, extent;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (!s->io->staging)
            return RPROTO_ST_ERR_STATE; /* no shared region on this transport */
        if (rproto_dec_blit(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        if (m.w == 0 || m.h == 0 ||
            m.w > RPROTO_MAX_DIM || m.h > RPROTO_MAX_DIM)
            return RPROTO_ST_ERR_ARG;
        /*
         * The client picks src_off, stride and the extent, so bound the
         * whole source rectangle against the staging area in 64 bits
         * before reading a single byte of it.
         */
        row_bytes = (uint64_t)m.w * 4;
        if (m.stride < row_bytes)
            return RPROTO_ST_ERR_ARG;
        extent = (uint64_t)(m.h - 1) * m.stride + row_bytes;
        if ((uint64_t)m.src_off + extent > s->io->staging_size)
            return RPROTO_ST_ERR_ARG;
        fb_blit(s, &m);
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_SET_BLEND: {
        struct rproto_set_blend m;

        if (!s->hello_done)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_set_blend(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        if (m.mode != RPROTO_BLEND_REPLACE && m.mode != RPROTO_BLEND_SRC_OVER)
            return RPROTO_ST_ERR_ARG;
        s->blend = m.mode;
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_DRAW_LINE: {
        struct rproto_draw_line m;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_draw_line(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        fb_line(s, &m);
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_DRAW_TRIANGLE: {
        struct rproto_draw_triangle m;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_draw_triangle(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        fb_triangle(s, &m);
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_PRESENT: {
        struct rproto_present m;

        if (!s->fb)
            return RPROTO_ST_ERR_STATE;
        if (rproto_dec_present(payload, hdr->payload_len, &m) < 0)
            return RPROTO_ST_ERR_PROTO;
        if (write_ppm(s, m.frame_id) < 0)
            return RPROTO_ST_ERR_INTERNAL;
        return RPROTO_ST_OK;
    }
    case RPROTO_MSG_GOODBYE:
        if (hdr->payload_len != RPROTO_LEN_GOODBYE)
            return RPROTO_ST_ERR_PROTO;
        *stop = 1;
        return RPROTO_ST_OK;
    default:
        return RPROTO_ST_ERR_PROTO;
    }
}

static void serve(struct rio *io)
{
    struct session s = { .io = io };
    uint8_t payload[RPROTO_MAX_PAYLOAD];
    struct rproto_hdr hdr;
    int stop = 0;

    /* Session must start with HELLO / HELLO_ACK version negotiation. */
    {
        struct rproto_hello hello;
        int r = io->recv(io->ctx, &hdr, payload, sizeof(payload));

        if (r != 0)
            goto out;
        if (hdr.type != RPROTO_MSG_HELLO ||
            rproto_dec_hello(payload, hdr.payload_len, &hello) < 0 ||
            hello.magic != RPROTO_MAGIC ||
            hello.ver_major != RPROTO_VER_MAJOR) {
            send_status(&s, RPROTO_ST_ERR_PROTO, hdr.seq);
            goto out;
        }
        struct rproto_hello_ack ack = {
            .magic = RPROTO_MAGIC,
            .ver_major = RPROTO_VER_MAJOR,
            .ver_minor = RPROTO_VER_MINOR,
            .max_payload = RPROTO_MAX_PAYLOAD,
        };
        uint8_t ackbuf[RPROTO_LEN_HELLO_ACK];
        uint32_t len = rproto_enc_hello_ack(ackbuf, &ack);

        if (io->send(io->ctx, RPROTO_MSG_HELLO_ACK, hdr.seq, ackbuf, len) < 0)
            goto out;
        s.hello_done = 1;
    }

    while (!stop) {
        int r = io->recv(io->ctx, &hdr, payload, sizeof(payload));

        if (r != 0)
            break;
        uint32_t st = handle_msg(&s, &hdr, payload, &stop);

        if (send_status(&s, st, hdr.seq) < 0)
            break;
    }
out:
    free(s.fb);
}

/* fd-based transports (unix / vsock / tcp) */
static int fd_recv(void *ctx, struct rproto_hdr *hdr, uint8_t *payload,
                   uint32_t cap)
{
    return rproto_recv(*(int *)ctx, hdr, payload, cap);
}

static int fd_send(void *ctx, uint32_t type, uint32_t seq,
                   const uint8_t *payload, uint32_t payload_len)
{
    return rproto_send(*(int *)ctx, type, seq, payload, payload_len);
}

/*
 * Shared-memory transport: requests arrive on g2h, replies go to h2g.
 * With a notifier attached (Phase 3) the waits become interrupt-driven;
 * without one (Phase 2) they poll.
 */
struct shm_io {
    struct rshm *shm;
    const struct rshm_notifier *notifier;
};

static int shm_recv(void *ctx, struct rproto_hdr *hdr, uint8_t *payload,
                    uint32_t cap)
{
    struct shm_io *io = ctx;

    return rshm_msg_recv_n(&io->shm->g2h, hdr, payload, cap, -1,
                           io->notifier);
}

static int shm_send(void *ctx, uint32_t type, uint32_t seq,
                    const uint8_t *payload, uint32_t payload_len)
{
    struct shm_io *io = ctx;
    int r = rshm_msg_send_n(&io->shm->h2g, type, seq, payload, payload_len,
                            -1, io->notifier);

    return r == 0 ? 0 : -1;
}

/* Phase 4: the vring itself carries request and reply, so the pair maps
 * straight onto the transport callbacks. */
static int vu_io_recv(void *ctx, struct rproto_hdr *hdr, uint8_t *payload,
                      uint32_t cap)
{
    return vu_recv(ctx, hdr, payload, cap);
}

static int vu_io_send(void *ctx, uint32_t type, uint32_t seq,
                      const uint8_t *payload, uint32_t payload_len)
{
    return vu_send_reply(ctx, type, seq, payload, payload_len);
}

/* Phase 3 notification, backed by the ivshmem server's eventfds. */
static int db_notify(void *ctx)
{
    struct ivshmem_client *cli = ctx;
    int64_t peer = ivshmem_client_first_peer(cli);

    if (peer == IVSHMEM_NO_PEER)
        return -1;
    return ivshmem_client_notify(cli, peer, 0);
}

static int db_wait(void *ctx, int timeout_ms)
{
    return ivshmem_client_wait_irq(ctx, 0, timeout_ms);
}

static int listen_unix(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "renderd: unix path too long\n");
        return -1;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("renderd: socket(AF_UNIX)");
        return -1;
    }
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 1) < 0) {
        perror("renderd: bind/listen unix");
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_vsock(uint32_t port)
{
    struct sockaddr_vm sa = {
        .svm_family = AF_VSOCK,
        .svm_cid = VMADDR_CID_ANY,
        .svm_port = port,
    };
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("renderd: socket(AF_VSOCK)");
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 1) < 0) {
        perror("renderd: bind/listen vsock");
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_tcp(uint16_t port)
{
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("renderd: socket(AF_INET)");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 1) < 0) {
        perror("renderd: bind/listen tcp");
        close(fd);
        return -1;
    }
    return fd;
}

/* Create (or reuse) FILE, size it, map it shared, and publish the layout.
 * QEMU maps the same file via memory-backend-file share=on. */
static void *setup_shm(const char *path, size_t size, struct rshm *shm)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    void *base;

    if (fd < 0) {
        fprintf(stderr, "renderd: open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        perror("renderd: ftruncate");
        close(fd);
        return NULL;
    }
    base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        perror("renderd: mmap");
        return NULL;
    }
    if (rshm_init(shm, base, size, -1) < 0) {
        fprintf(stderr, "renderd: shm region too small\n");
        munmap(base, size);
        return NULL;
    }
    return base;
}

/*
 * Wait until the reply ring has drained, so a session's last STATUS is
 * not wiped by the reset that follows it.
 */
static void drain_replies(struct rshm *shm)
{
    for (int i = 0; i < 500; i++) {
        uint32_t prod = __atomic_load_n(&shm->h2g.hdr->prod, __ATOMIC_ACQUIRE);
        uint32_t cons = __atomic_load_n(&shm->h2g.hdr->cons, __ATOMIC_ACQUIRE);

        if (prod == cons)
            return;
        usleep(10 * 1000);
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: renderd (--unix PATH | --vsock PORT | --tcp PORT |"
            " --shm FILE | --ivshmem SOCKET | --vhost-user SOCKET)"
            " [--out DIR] [--once]\n");
}

int main(int argc, char **argv)
{
    const char *unix_path = NULL;
    const char *shm_path = NULL;
    const char *ivshmem_path = NULL;
    const char *vhost_user_path = NULL;
    long vsock_port = -1;
    long tcp_port = -1;
    int once = 0;
    int lfd;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
            unix_path = argv[++i];
        } else if (strcmp(argv[i], "--vsock") == 0 && i + 1 < argc) {
            vsock_port = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
            tcp_port = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            shm_path = argv[++i];
        } else if (strcmp(argv[i], "--ivshmem") == 0 && i + 1 < argc) {
            ivshmem_path = argv[++i];
        } else if (strcmp(argv[i], "--vhost-user") == 0 && i + 1 < argc) {
            vhost_user_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_outdir = argv[++i];
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else {
            usage();
            return 2;
        }
    }
    if ((unix_path != NULL) + (vsock_port >= 0) + (tcp_port >= 0) +
            (shm_path != NULL) + (ivshmem_path != NULL) +
            (vhost_user_path != NULL) != 1) {
        usage();
        return 2;
    }
    if (tcp_port >= 0 && (tcp_port == 0 || tcp_port > 65535)) {
        usage();
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    if (mkdir(g_outdir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "renderd: mkdir %s: %s\n", g_outdir, strerror(errno));
        return 1;
    }

    if (shm_path) {
        struct rshm shm;
        struct shm_io sio = { .shm = &shm, .notifier = NULL };
        struct rio io = { .recv = shm_recv, .send = shm_send, .ctx = &sio };

        if (!setup_shm(shm_path, RSHM_DEFAULT_SIZE, &shm))
            return 1;
        io.staging = (const uint8_t *)shm.hdr + shm.hdr->fb_off;
        io.staging_size = shm.hdr->fb_size;
        fprintf(stderr, "renderd: serving shm region %s\n", shm_path);
        do {
            serve(&io);
            drain_replies(&shm);
            fprintf(stderr, "renderd: shm session ended\n");
            rshm_reset_rings(&shm);
        } while (!once);
        return 0;
    }

    if (vhost_user_path) {
        static struct vu_dev vu;
        struct rio io = { .recv = vu_io_recv, .send = vu_io_send, .ctx = &vu };

        if (vu_listen(&vu, vhost_user_path) < 0) {
            fprintf(stderr, "renderd: cannot listen on %s\n", vhost_user_path);
            return 1;
        }
        fprintf(stderr, "renderd: vhost-user backend on %s\n",
                vhost_user_path);
        do {
            serve(&io);
            fprintf(stderr, "renderd: vhost-user session ended\n");
        } while (!once);
        vu_close(&vu);
        unlink(vhost_user_path);
        return 0;
    }

    if (ivshmem_path) {
        struct ivshmem_client cli;
        struct rshm shm;
        struct rshm_notifier notifier = {
            .notify = db_notify, .wait = db_wait, .ctx = &cli,
        };
        struct shm_io sio = { .shm = &shm, .notifier = &notifier };
        struct rio io = { .recv = shm_recv, .send = shm_send, .ctx = &sio };
        struct stat st;
        void *base;

        if (ivshmem_client_connect(&cli, ivshmem_path) < 0) {
            fprintf(stderr, "renderd: cannot join ivshmem server %s\n",
                    ivshmem_path);
            return 1;
        }
        if (fstat(cli.shm_fd, &st) < 0 || st.st_size <= 0) {
            perror("renderd: fstat ivshmem region");
            ivshmem_client_close(&cli);
            return 1;
        }
        base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, cli.shm_fd, 0);
        if (base == MAP_FAILED) {
            perror("renderd: mmap ivshmem region");
            ivshmem_client_close(&cli);
            return 1;
        }
        /* Publish our peer ID with the layout: the guest needs it to aim
         * the doorbell back at us. */
        if (rshm_init(&shm, base, (size_t)st.st_size, (int32_t)cli.id) < 0) {
            fprintf(stderr, "renderd: ivshmem region too small\n");
            ivshmem_client_close(&cli);
            return 1;
        }
        io.staging = (const uint8_t *)shm.hdr + shm.hdr->fb_off;
        io.staging_size = shm.hdr->fb_size;
        fprintf(stderr, "renderd: joined %s as peer %lld,"
                " %lld bytes shared, doorbell-driven\n",
                ivshmem_path, (long long)cli.id, (long long)st.st_size);
        do {
            serve(&io);
            drain_replies(&shm);
            fprintf(stderr, "renderd: ivshmem session ended\n");
            rshm_reset_rings(&shm);
        } while (!once);
        ivshmem_client_close(&cli);
        return 0;
    }

    if (unix_path)
        lfd = listen_unix(unix_path);
    else if (vsock_port >= 0)
        lfd = listen_vsock((uint32_t)vsock_port);
    else
        lfd = listen_tcp((uint16_t)tcp_port);
    if (lfd < 0)
        return 1;
    fprintf(stderr, "renderd: listening on %s\n",
            unix_path ? unix_path : (vsock_port >= 0 ? "vsock" : "tcp"));

    do {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("renderd: accept");
            break;
        }
        fprintf(stderr, "renderd: client connected\n");
        {
            struct rio io = { .recv = fd_recv, .send = fd_send, .ctx = &cfd };

            serve(&io);
        }
        close(cfd);
        fprintf(stderr, "renderd: client disconnected\n");
    } while (!once);

    close(lfd);
    if (unix_path)
        unlink(unix_path);
    return 0;
}
