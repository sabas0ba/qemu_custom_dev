/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "rproto.h"
#include "render_client.h"
#include "ivshmem_rproto.h"

/* How long connect/handshake waits for the host side, in ms. */
#define RC_SHM_ATTACH_TIMEOUT 5000
#define RC_SHM_IO_TIMEOUT     10000

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
    memset(rc, 0, sizeof(*rc));
    rc->fd = fd;
    rc->db.fd = -1;
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
    memset(rc, 0, sizeof(*rc));
    rc->fd = fd;
    rc->db.fd = -1;
    rc->next_seq = 1;
    return 0;
}

int rc_connect_tcp(struct render_client *rc, const char *addr, uint16_t port)
{
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    int fd;

    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    memset(rc, 0, sizeof(*rc));
    rc->fd = fd;
    rc->db.fd = -1;
    rc->next_seq = 1;
    return 0;
}

static int rc_map_shm(struct render_client *rc, int fd, size_t size)
{
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED)
        return -1;
    memset(rc, 0, sizeof(*rc));
    rc->db.fd = -1;
    if (rshm_attach(&rc->shm, base, size, RC_SHM_ATTACH_TIMEOUT) < 0) {
        fprintf(stderr, "render_client: no valid shm layout found"
                " (is renderd --shm running?)\n");
        munmap(base, size);
        return -1;
    }
    rc->shm_base = base;
    rc->shm_size = size;
    rc->use_shm = 1;
    rc->fd = -1;
    rc->next_seq = 1;
    return 0;
}

int rc_attach_shm(struct render_client *rc, void *base, size_t size,
                  const struct rshm_notifier *n)
{
    memset(rc, 0, sizeof(*rc));
    rc->fd = -1;
    rc->db.fd = -1;
    if (rshm_attach(&rc->shm, base, size, RC_SHM_ATTACH_TIMEOUT) < 0)
        return -1;
    rc->use_shm = 1;
    rc->next_seq = 1;
    /* caller owns the mapping: leave shm_base NULL so rc_close keeps it */
    if (n) {
        rc->notifier = *n;
        rc->notifier_p = &rc->notifier;
    }
    return 0;
}

int rc_connect_shm_file(struct render_client *rc, const char *path)
{
    struct stat st;
    int fd = open(path, O_RDWR);
    int r;

    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    r = rc_map_shm(rc, fd, (size_t)st.st_size);
    close(fd);
    return r;
}

/* Read a sysfs hex id file like "0x1af4\n". Returns -1 on error. */
static long read_sysfs_id(const char *dir, const char *file)
{
    char path[512];
    char buf[32];
    FILE *f;
    long v;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    v = strtol(buf, NULL, 16);
    return v;
}

int rc_connect_shm_pci(struct render_client *rc)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    char dev_dir[384];
    int found = 0;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(dev_dir, sizeof(dev_dir), "/sys/bus/pci/devices/%s",
                 e->d_name);
        if (read_sysfs_id(dev_dir, "vendor") == 0x1af4 &&
            read_sysfs_id(dev_dir, "device") == 0x1110) {
            found = 1;
            break;
        }
    }
    closedir(d);
    if (!found) {
        fprintf(stderr, "render_client: no ivshmem device (1af4:1110)"
                " on the PCI bus\n");
        return -1;
    }
    fprintf(stderr, "render_client: using ivshmem at %s\n", dev_dir);

    /* Make sure memory decode is on; without a bound driver it may not
     * be. Ignore failures (it may already be enabled). */
    {
        char path[512];
        FILE *f;

        snprintf(path, sizeof(path), "%s/enable", dev_dir);
        f = fopen(path, "w");
        if (f) {
            fputs("1", f);
            fclose(f);
        }
    }

    {
        char path[512];
        struct stat st;
        int fd, r;

        snprintf(path, sizeof(path), "%s/resource2", dev_dir);
        fd = open(path, O_RDWR);
        if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0) {
            fprintf(stderr, "render_client: cannot open %s"
                    " (root required)\n", path);
            if (fd >= 0)
                close(fd);
            return -1;
        }
        r = rc_map_shm(rc, fd, (size_t)st.st_size);
        close(fd);
        return r;
    }
}

/* --- Phase 3: doorbell notification through the kernel module --- */

static int rc_db_notify(void *ctx)
{
    struct rc_doorbell *db = ctx;

    if (ioctl(db->fd, IVSHM_IOC_RING, &db->ring_value) < 0)
        return -1;
    return 0;
}

static int rc_db_wait(void *ctx, int timeout_ms)
{
    struct rc_doorbell *db = ctx;
    struct pollfd pfd = { .fd = db->fd, .events = POLLIN };
    uint32_t count;
    int r;

    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    if (r == 0)
        return 1;
    /* Consume the event so the next wait blocks again. */
    if (read(db->fd, &count, sizeof(count)) != (ssize_t)sizeof(count))
        return -1;
    return 0;
}

int rc_connect_doorbell(struct render_client *rc)
{
    uint64_t shm_size = 0;
    int32_t host_id;
    int fd = open(IVSHM_RPROTO_DEVPATH, O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "render_client: cannot open %s: %s\n"
                " (load the ivshmem_rproto module; root required)\n",
                IVSHM_RPROTO_DEVPATH, strerror(errno));
        return -1;
    }
    if (ioctl(fd, IVSHM_IOC_SHM_SIZE, &shm_size) < 0 || shm_size == 0) {
        fprintf(stderr, "render_client: cannot query shm size\n");
        close(fd);
        return -1;
    }
    if (rc_map_shm(rc, fd, (size_t)shm_size) < 0) {
        close(fd);
        return -1;
    }

    host_id = rc->shm.hdr->host_peer_id;
    if (host_id < 0) {
        fprintf(stderr, "render_client: host published no peer id;"
                " is renderd running with --ivshmem?\n");
        munmap(rc->shm_base, rc->shm_size);
        rc->use_shm = 0;
        close(fd);
        return -1;
    }
    rc->db.fd = fd;
    /* vector 0: the only one the driver asks the device for */
    rc->db.ring_value = ((uint32_t)host_id << 16) | 0u;
    rc->notifier.notify = rc_db_notify;
    rc->notifier.wait = rc_db_wait;
    rc->notifier.ctx = &rc->db;
    rc->notifier_p = &rc->notifier;
    fprintf(stderr, "render_client: doorbell transport ready"
            " (host peer %d)\n", host_id);
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

    if (rc->use_shm) {
        if (rshm_msg_send_n(&rc->shm.g2h, type, seq, payload, payload_len,
                            RC_SHM_IO_TIMEOUT, rc->notifier_p) != 0)
            return -1;
        if (rshm_msg_recv_n(&rc->shm.h2g, &hdr, rbuf, sizeof(rbuf),
                            RC_SHM_IO_TIMEOUT, rc->notifier_p) != 0)
            return -1;
    } else {
        if (rproto_send(rc->fd, type, seq, payload, payload_len) < 0)
            return -1;
        if (rproto_recv(rc->fd, &hdr, rbuf, sizeof(rbuf)) != 0)
            return -1;
    }
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

    if (rc->use_shm) {
        if (rshm_msg_send_n(&rc->shm.g2h, RPROTO_MSG_HELLO, seq, buf, len,
                            RC_SHM_IO_TIMEOUT, rc->notifier_p) != 0)
            return -1;
        if (rshm_msg_recv_n(&rc->shm.h2g, &hdr, buf, sizeof(buf),
                            RC_SHM_IO_TIMEOUT, rc->notifier_p) != 0)
            return -1;
    } else {
        if (rproto_send(rc->fd, RPROTO_MSG_HELLO, seq, buf, len) < 0)
            return -1;
        if (rproto_recv(rc->fd, &hdr, buf, sizeof(buf)) != 0)
            return -1;
    }
    if (hdr.type != RPROTO_MSG_HELLO_ACK ||
        rproto_dec_hello_ack(buf, hdr.payload_len, &ack) < 0 ||
        ack.magic != RPROTO_MAGIC || ack.ver_major != RPROTO_VER_MAJOR)
        return -1;
    /* Minor versions only add messages, so remember what this server
     * supports rather than refusing to talk to an older one. */
    rc->server_minor = ack.ver_minor;
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

void *rc_staging(struct render_client *rc, size_t *size)
{
    if (!rc->use_shm)
        return NULL;
    if (size)
        *size = rc->shm.hdr->fb_size;
    return (uint8_t *)rc->shm.hdr + rc->shm.hdr->fb_off;
}

int rc_blit(struct render_client *rc, uint32_t src_off, uint32_t stride,
            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint8_t buf[RPROTO_LEN_BLIT];
    struct rproto_blit m = { .src_off = src_off, .stride = stride,
                             .x = x, .y = y, .w = w, .h = h };

    if (!rc->use_shm) {
        fprintf(stderr, "render_client: BLIT needs a shared-memory"
                " transport\n");
        return -1;
    }
    if (rc->server_minor < 2) {
        fprintf(stderr, "render_client: server speaks v0.%u, BLIT needs"
                " v0.2\n", rc->server_minor);
        return -1;
    }
    return rc_call(rc, RPROTO_MSG_BLIT, buf, rproto_enc_blit(buf, &m));
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

int rc_draw_demo_scene(struct render_client *rc, uint32_t frame_id)
{
    if (rc_create_surface(rc, 320, 240) < 0 ||
        rc_clear(rc, 0x102030ff) < 0 ||
        rc_fill_rect(rc, 40, 40, 80, 60, 0xff0000ff) < 0 ||
        rc_fill_rect(rc, 160, 120, 100, 80, 0x00ff00ff) < 0 ||
        rc_present(rc, frame_id) < 0)
        return -1;
    return 0;
}

int rc_set_blend(struct render_client *rc, uint32_t mode)
{
    uint8_t buf[RPROTO_LEN_SET_BLEND];
    struct rproto_set_blend m = { .mode = mode };

    if (rc->server_minor < 3) {
        fprintf(stderr, "render_client: server speaks v0.%u, blending needs"
                " v0.3\n", rc->server_minor);
        return -1;
    }
    return rc_call(rc, RPROTO_MSG_SET_BLEND, buf,
                   rproto_enc_set_blend(buf, &m));
}

int rc_draw_line(struct render_client *rc, int32_t x0, int32_t y0,
                 int32_t x1, int32_t y1, uint32_t rgba)
{
    uint8_t buf[RPROTO_LEN_DRAW_LINE];
    struct rproto_draw_line m = { .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
                                  .rgba = rgba };

    if (rc->server_minor < 3) {
        fprintf(stderr, "render_client: server speaks v0.%u, DRAW_LINE needs"
                " v0.3\n", rc->server_minor);
        return -1;
    }
    return rc_call(rc, RPROTO_MSG_DRAW_LINE, buf,
                   rproto_enc_draw_line(buf, &m));
}

int rc_draw_triangle(struct render_client *rc, int32_t x0, int32_t y0,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                     uint32_t rgba)
{
    uint8_t buf[RPROTO_LEN_DRAW_TRIANGLE];
    struct rproto_draw_triangle m = { .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
                                      .x2 = x2, .y2 = y2, .rgba = rgba };

    if (rc->server_minor < 3) {
        fprintf(stderr, "render_client: server speaks v0.%u, DRAW_TRIANGLE"
                " needs v0.3\n", rc->server_minor);
        return -1;
    }
    return rc_call(rc, RPROTO_MSG_DRAW_TRIANGLE, buf,
                   rproto_enc_draw_triangle(buf, &m));
}

int rc_draw_rich_scene(struct render_client *rc, uint32_t frame_id)
{
    /* Order matters: the triangle and the line are laid down opaque, then
     * the mode switches and the rectangle composites over both. */
    if (rc_create_surface(rc, 320, 240) < 0 ||
        rc_clear(rc, 0x102030ff) < 0 ||
        rc_draw_triangle(rc, 160, 20, 40, 180, 280, 180, 0x00c000ff) < 0 ||
        rc_draw_line(rc, 20, 200, 300, 200, 0xffffffff) < 0 ||
        rc_set_blend(rc, RPROTO_BLEND_SRC_OVER) < 0 ||
        rc_fill_rect(rc, 140, 60, 80, 50, 0xff000080) < 0 ||
        rc_present(rc, frame_id) < 0)
        return -1;
    return 0;
}

/* Fill n pixels of staging with one R,G,B,A colour. */
static void stage_solid(uint8_t *dst, size_t n, uint32_t rgba)
{
    for (size_t i = 0; i < n; i++, dst += 4) {
        dst[0] = (uint8_t)((rgba >> 24) & 0xff);
        dst[1] = (uint8_t)((rgba >> 16) & 0xff);
        dst[2] = (uint8_t)((rgba >> 8) & 0xff);
        dst[3] = (uint8_t)(rgba & 0xff);
    }
}

int rc_draw_blit_scene(struct render_client *rc, uint32_t frame_id)
{
    size_t staging_size = 0;
    uint8_t *staging = rc_staging(rc, &staging_size);
    /* same two rectangles as rc_draw_demo_scene */
    const uint32_t red_px = 80u * 60u, green_px = 100u * 80u;
    const uint32_t green_off = red_px * 4u;

    if (!staging) {
        fprintf(stderr, "render_client: no staging area on this"
                " transport\n");
        return -1;
    }
    if (staging_size < (size_t)green_off + green_px * 4u) {
        fprintf(stderr, "render_client: staging area too small\n");
        return -1;
    }
    stage_solid(staging, red_px, 0xff0000ff);
    stage_solid(staging + green_off, green_px, 0x00ff00ff);

    if (rc_create_surface(rc, 320, 240) < 0 ||
        rc_clear(rc, 0x102030ff) < 0 ||
        rc_blit(rc, 0, 80u * 4u, 40, 40, 80, 60) < 0 ||
        rc_blit(rc, green_off, 100u * 4u, 160, 120, 100, 80) < 0 ||
        rc_present(rc, frame_id) < 0)
        return -1;
    return 0;
}

void rc_close(struct render_client *rc)
{
    if (rc->db.fd >= 0) {
        close(rc->db.fd);
        rc->db.fd = -1;
        rc->notifier_p = NULL;
    }
    if (rc->use_shm) {
        if (rc->shm_base)
            munmap(rc->shm_base, rc->shm_size);
        rc->shm_base = NULL;
        rc->use_shm = 0;
    }
    if (rc->fd >= 0) {
        close(rc->fd);
        rc->fd = -1;
    }
}

static const char *g_transport_usage =
    "--unix PATH | --vsock CID PORT | --tcp ADDR PORT"
    " | --shm-file PATH | --shm-pci | --doorbell";

const char *rc_transport_usage(void)
{
    return g_transport_usage;
}

int rc_connect_argv(struct render_client *rc, int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[0], "--unix") == 0)
        return rc_connect_unix(rc, argv[1]) == 0 ? 2 : -1;
    if (argc >= 3 && strcmp(argv[0], "--vsock") == 0)
        return rc_connect_vsock(rc, (uint32_t)strtoul(argv[1], NULL, 10),
                                (uint32_t)strtoul(argv[2], NULL, 10)) == 0
               ? 3 : -1;
    if (argc >= 3 && strcmp(argv[0], "--tcp") == 0)
        return rc_connect_tcp(rc, argv[1],
                              (uint16_t)strtoul(argv[2], NULL, 10)) == 0
               ? 3 : -1;
    if (argc >= 2 && strcmp(argv[0], "--shm-file") == 0)
        return rc_connect_shm_file(rc, argv[1]) == 0 ? 2 : -1;
    if (argc >= 1 && strcmp(argv[0], "--shm-pci") == 0)
        return rc_connect_shm_pci(rc) == 0 ? 1 : -1;
    if (argc >= 1 && strcmp(argv[0], "--doorbell") == 0)
        return rc_connect_doorbell(rc) == 0 ? 1 : -1;

    fprintf(stderr, "render_client: expected a transport: %s\n",
            g_transport_usage);
    return -1;
}
