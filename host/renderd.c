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
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "rproto.h"

struct session {
    int fd;
    int hello_done;
    uint32_t width;
    uint32_t height;
    uint32_t *fb; /* width*height pixels, 0xRRGGBBAA */
};

static const char *g_outdir = ".";

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
        for (uint32_t col = x; col < x + w; col++)
            s->fb[(size_t)row * s->width + col] = rgba;
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

    return rproto_send(s->fd, RPROTO_MSG_STATUS, 0, payload, len);
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

static void serve(int cfd)
{
    struct session s = { .fd = cfd };
    uint8_t payload[RPROTO_MAX_PAYLOAD];
    struct rproto_hdr hdr;
    int stop = 0;

    /* Session must start with HELLO / HELLO_ACK version negotiation. */
    {
        struct rproto_hello hello;
        int r = rproto_recv(cfd, &hdr, payload, sizeof(payload));

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

        if (rproto_send(cfd, RPROTO_MSG_HELLO_ACK, hdr.seq, ackbuf, len) < 0)
            goto out;
        s.hello_done = 1;
    }

    while (!stop) {
        int r = rproto_recv(cfd, &hdr, payload, sizeof(payload));

        if (r != 0)
            break;
        uint32_t st = handle_msg(&s, &hdr, payload, &stop);

        if (send_status(&s, st, hdr.seq) < 0)
            break;
    }
out:
    free(s.fb);
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

static void usage(void)
{
    fprintf(stderr,
            "usage: renderd (--unix PATH | --vsock PORT) [--out DIR] [--once]\n");
}

int main(int argc, char **argv)
{
    const char *unix_path = NULL;
    long vsock_port = -1;
    int once = 0;
    int lfd;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
            unix_path = argv[++i];
        } else if (strcmp(argv[i], "--vsock") == 0 && i + 1 < argc) {
            vsock_port = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_outdir = argv[++i];
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else {
            usage();
            return 2;
        }
    }
    if ((unix_path == NULL) == (vsock_port < 0)) {
        usage();
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    if (mkdir(g_outdir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "renderd: mkdir %s: %s\n", g_outdir, strerror(errno));
        return 1;
    }

    lfd = unix_path ? listen_unix(unix_path) : listen_vsock((uint32_t)vsock_port);
    if (lfd < 0)
        return 1;
    fprintf(stderr, "renderd: listening on %s\n",
            unix_path ? unix_path : "vsock");

    do {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("renderd: accept");
            break;
        }
        fprintf(stderr, "renderd: client connected\n");
        serve(cfd);
        close(cfd);
        fprintf(stderr, "renderd: client disconnected\n");
    } while (!once);

    close(lfd);
    if (unix_path)
        unlink(unix_path);
    return 0;
}
