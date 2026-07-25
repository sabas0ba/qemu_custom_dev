#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
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
    rc->next_seq = 1;
    return 0;
}

static int rc_map_shm(struct render_client *rc, int fd, size_t size)
{
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED)
        return -1;
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

/* Send one request and wait for the matching STATUS reply. */
static int rc_call(struct render_client *rc, uint32_t type,
                   const uint8_t *payload, uint32_t payload_len)
{
    uint8_t rbuf[RPROTO_MAX_PAYLOAD];
    struct rproto_hdr hdr;
    struct rproto_status_msg st;
    uint32_t seq = rc->next_seq++;

    if (rc->use_shm) {
        if (rshm_msg_send(&rc->shm.g2h, type, seq, payload, payload_len,
                          RC_SHM_IO_TIMEOUT) != 0)
            return -1;
        if (rshm_msg_recv(&rc->shm.h2g, &hdr, rbuf, sizeof(rbuf),
                          RC_SHM_IO_TIMEOUT) != 0)
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
        if (rshm_msg_send(&rc->shm.g2h, RPROTO_MSG_HELLO, seq, buf, len,
                          RC_SHM_IO_TIMEOUT) != 0)
            return -1;
        if (rshm_msg_recv(&rc->shm.h2g, &hdr, buf, sizeof(buf),
                          RC_SHM_IO_TIMEOUT) != 0)
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
    if (rc->use_shm) {
        munmap(rc->shm_base, rc->shm_size);
        rc->shm_base = NULL;
        rc->use_shm = 0;
    }
    if (rc->fd >= 0) {
        close(rc->fd);
        rc->fd = -1;
    }
}
