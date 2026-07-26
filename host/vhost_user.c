/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vhost_user.h"

/* Which chain vu_recv() took, so vu_send_reply() can complete it. */
static struct {
    int valid;
    uint16_t head;
    uint8_t *resp;
    uint32_t resp_cap;
} g_pending;

/* ---------------------------------------------------------------- I/O */

static int recv_msg(int fd, struct vhost_user_hdr *hdr, void *payload,
                    size_t cap, int *fds, int *nfds)
{
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(*hdr) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_RAM_SLOTS)];
    } cmsg_u;
    struct cmsghdr *cmsg;
    ssize_t n;

    *nfds = 0;
    memset(&cmsg_u, 0, sizeof(cmsg_u));
    msg.msg_control = cmsg_u.buf;
    msg.msg_controllen = sizeof(cmsg_u.buf);

    do {
        n = recvmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n == 0)
        return 1; /* frontend closed */
    if (n != (ssize_t)sizeof(*hdr)) {
        if (getenv("VU_DEBUG"))
            fprintf(stderr, "vhost-user: short header read %zd: %s\n",
                    n, n < 0 ? strerror(errno) : "truncated");
        return -1;
    }

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t len = cmsg->cmsg_len - CMSG_LEN(0);

            *nfds = (int)(len / sizeof(int));
            if (*nfds > VHOST_USER_MAX_RAM_SLOTS)
                *nfds = VHOST_USER_MAX_RAM_SLOTS;
            memcpy(fds, CMSG_DATA(cmsg), (size_t)*nfds * sizeof(int));
        }
    }

    if (hdr->size > cap)
        return -1;
    for (uint32_t got = 0; got < hdr->size; ) {
        ssize_t r = read(fd, (uint8_t *)payload + got, hdr->size - got);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        got += (uint32_t)r;
    }
    return 0;
}

static int send_reply(int fd, const struct vhost_user_hdr *req,
                      const void *payload, uint32_t size)
{
    struct vhost_user_hdr hdr = {
        .request = req->request,
        .flags = (req->flags & VHOST_USER_VERSION_MASK) | VHOST_USER_REPLY_MASK,
        .size = size,
    };
    struct iovec iov[2] = {
        { .iov_base = &hdr, .iov_len = sizeof(hdr) },
        { .iov_base = (void *)payload, .iov_len = size },
    };
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = size ? 2 : 1 };
    ssize_t n;

    do {
        n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)(sizeof(hdr) + size) ? 0 : -1;
}

/* ------------------------------------------------------ address maps */

/* QEMU userspace address -> our mapping. */
static void *from_qva(struct vu_dev *d, uint64_t qva, uint64_t len)
{
    for (int i = 0; i < d->nregions; i++) {
        struct vu_region *r = &d->regions[i];

        if (qva >= r->qva && qva + len <= r->qva + r->size)
            return r->base + (qva - r->qva);
    }
    return NULL;
}

/* Guest physical address -> our mapping. Descriptor addresses are these. */
static void *from_gpa(struct vu_dev *d, uint64_t gpa, uint64_t len)
{
    for (int i = 0; i < d->nregions; i++) {
        struct vu_region *r = &d->regions[i];

        if (gpa >= r->gpa && gpa + len <= r->gpa + r->size)
            return r->base + (gpa - r->gpa);
    }
    return NULL;
}

static void unmap_regions(struct vu_dev *d)
{
    for (int i = 0; i < d->nregions; i++)
        if (d->regions[i].mmap_addr)
            munmap(d->regions[i].mmap_addr, d->regions[i].mmap_size);
    d->nregions = 0;
    d->running = 0;
    d->vq.desc = NULL;
    d->vq.avail = NULL;
    d->vq.used = NULL;
}

static int set_mem_table(struct vu_dev *d, const struct vhost_user_memory *mem,
                         int *fds, int nfds)
{
    unmap_regions(d);
    if (mem->nregions > VHOST_USER_MAX_RAM_SLOTS ||
        (int)mem->nregions > nfds)
        return -1;

    for (uint32_t i = 0; i < mem->nregions; i++) {
        const struct vhost_user_mem_region *src = &mem->regions[i];
        struct vu_region *r = &d->regions[i];
        size_t len = (size_t)(src->memory_size + src->mmap_offset);
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fds[i], 0);

        if (p == MAP_FAILED) {
            fprintf(stderr, "vhost-user: mmap region %u: %s\n", i,
                    strerror(errno));
            return -1;
        }
        r->gpa = src->guest_phys_addr;
        r->qva = src->userspace_addr;
        r->size = src->memory_size;
        r->mmap_addr = p;
        r->mmap_size = len;
        r->base = (uint8_t *)p + src->mmap_offset;
        d->nregions = (int)i + 1;
    }
    return 0;
}

/* Translate the vring once both the memory table and the addresses are in. */
static void vring_ready(struct vu_dev *d)
{
    struct vu_vring *v = &d->vq;

    if (!v->num || !v->desc_qva || d->nregions == 0)
        return;
    v->desc = from_qva(d, v->desc_qva, (uint64_t)v->num * 16);
    v->avail = from_qva(d, v->avail_qva, 6 + (uint64_t)v->num * 2);
    v->used = from_qva(d, v->used_qva, 6 + (uint64_t)v->num * 8);
    if (v->desc && v->avail && v->used && v->enabled && v->kick_fd >= 0) {
        d->running = 1;
        fprintf(stderr, "vhost-user: queue ready (%u descriptors)\n", v->num);
    }
}

/* --------------------------------------------------- request handling */

/* Handle one vhost-user message. Returns 0, 1 on disconnect, -1 on error. */
static int handle_message(struct vu_dev *d)
{
    struct vhost_user_hdr hdr;
    union {
        uint64_t u64;
        struct vhost_user_memory mem;
        struct vhost_user_vring_state state;
        struct vhost_user_vring_addr addr;
        uint8_t raw[1024];
    } p;
    int fds[VHOST_USER_MAX_RAM_SLOTS];
    int nfds = 0;
    int r = recv_msg(d->conn_fd, &hdr, &p, sizeof(p), fds, &nfds);

    if (r != 0)
        return r;
    if (getenv("VU_DEBUG"))
        fprintf(stderr, "vhost-user: request %u flags 0x%x size %u fds %d\n",
                hdr.request, hdr.flags, hdr.size, nfds);

    switch (hdr.request) {
    case VHOST_USER_GET_FEATURES: {
        /*
         * Only what this backend truly implements: modern virtio, plus
         * the protocol-features handshake. Leaving out EVENT_IDX and
         * INDIRECT_DESC keeps the vring walk to the plain case.
         */
        uint64_t f = (1ull << VIRTIO_F_VERSION_1) | (1ull << 30);

        return send_reply(d->conn_fd, &hdr, &f, sizeof(f));
    }
    case VHOST_USER_SET_FEATURES:
        d->features = p.u64;
        break;
    case VHOST_USER_GET_PROTOCOL_FEATURES: {
        uint64_t f = 0; /* none needed for a single plain queue */

        return send_reply(d->conn_fd, &hdr, &f, sizeof(f));
    }
    case VHOST_USER_SET_PROTOCOL_FEATURES:
        break;
    case VHOST_USER_GET_QUEUE_NUM: {
        uint64_t n = 1;

        return send_reply(d->conn_fd, &hdr, &n, sizeof(n));
    }
    case VHOST_USER_SET_OWNER:
    case VHOST_USER_RESET_OWNER:
        break;
    case VHOST_USER_SET_MEM_TABLE: {
        int rc = set_mem_table(d, &p.mem, fds, nfds);

        for (int i = 0; i < nfds; i++)
            close(fds[i]);
        if (rc < 0)
            return rc;
        vring_ready(d);
        break;
    }
    case VHOST_USER_SET_VRING_NUM:
        d->vq.num = p.state.num;
        vring_ready(d);
        break;
    case VHOST_USER_SET_VRING_ADDR:
        d->vq.desc_qva = p.addr.desc_user_addr;
        d->vq.avail_qva = p.addr.avail_user_addr;
        d->vq.used_qva = p.addr.used_user_addr;
        vring_ready(d);
        break;
    case VHOST_USER_SET_VRING_BASE:
        d->vq.last_avail = (uint16_t)p.state.num;
        break;
    case VHOST_USER_GET_VRING_BASE: {
        struct vhost_user_vring_state s = {
            .index = p.state.index, .num = d->vq.last_avail,
        };

        /*
         * The frontend is taking the queue away again: stop walking it
         * and give back the kick fd, or the next start would poll a
         * stale eventfd.
         */
        d->running = 0;
        if (d->vq.kick_fd >= 0) {
            close(d->vq.kick_fd);
            d->vq.kick_fd = -1;
        }
        return send_reply(d->conn_fd, &hdr, &s, sizeof(s));
    }
    case VHOST_USER_SET_VRING_KICK:
        if (d->vq.kick_fd >= 0)
            close(d->vq.kick_fd);
        /* bit 8 set means "no fd, poll instead"; we require a real one */
        d->vq.kick_fd = (p.u64 & 0x100) ? -1 : (nfds > 0 ? fds[0] : -1);
        vring_ready(d);
        break;
    case VHOST_USER_SET_VRING_CALL:
        if (d->vq.call_fd >= 0)
            close(d->vq.call_fd);
        d->vq.call_fd = (p.u64 & 0x100) ? -1 : (nfds > 0 ? fds[0] : -1);
        break;
    case VHOST_USER_SET_VRING_ERR:
        if (nfds > 0)
            close(fds[0]);
        break;
    case VHOST_USER_SET_VRING_ENABLE:
        d->vq.enabled = p.state.num != 0;
        if (!d->vq.enabled)
            d->running = 0;
        vring_ready(d);
        break;
    default:
        /* Anything else is optional: drop it, but never leak its fds. */
        for (int i = 0; i < nfds; i++)
            close(fds[i]);
        if (!(hdr.flags & VHOST_USER_NEED_REPLY_MASK))
            fprintf(stderr, "vhost-user: ignoring request %u\n", hdr.request);
        break;
    }

    /*
     * Requests that carry no reply of their own still get one when the
     * frontend set NEED_REPLY (it does that for anything it wants
     * acknowledged, e.g. SET_VRING_ENABLE); 0 means success.
     */
    if (hdr.flags & VHOST_USER_NEED_REPLY_MASK) {
        uint64_t zero = 0;

        return send_reply(d->conn_fd, &hdr, &zero, sizeof(zero));
    }
    return 0;
}

/* ------------------------------------------------------------ vring */

/*
 * Pop one available chain. Copies the readable descriptors into buf and
 * records the first writable one as the reply area. Returns the number of
 * request bytes, or -1 when nothing is pending.
 */
static long pop_chain(struct vu_dev *d, uint8_t *buf, uint32_t cap)
{
    struct vu_vring *v = &d->vq;
    uint16_t avail_idx = __atomic_load_n(&v->avail->idx, __ATOMIC_ACQUIRE);
    uint16_t head, i;
    uint32_t got = 0;
    int guard = 0;

    if (avail_idx == v->last_avail)
        return -1;
    head = v->avail->ring[v->last_avail % v->num];
    if (head >= v->num)
        return -1;

    g_pending.valid = 0;
    g_pending.head = head;
    g_pending.resp = NULL;
    g_pending.resp_cap = 0;

    for (i = head; guard++ < (int)v->num; ) {
        struct vring_desc_raw *desc = &v->desc[i];
        void *p = from_gpa(d, desc->addr, desc->len);

        if (!p) {
            fprintf(stderr, "vhost-user: descriptor outside guest memory\n");
            return -1;
        }
        if (desc->flags & VRING_DESC_F_WRITE) {
            if (!g_pending.resp) {
                g_pending.resp = p;
                g_pending.resp_cap = desc->len;
            }
        } else {
            uint32_t n = desc->len;

            if (got + n > cap)
                return -1;
            memcpy(buf + got, p, n);
            got += n;
        }
        if (!(desc->flags & VRING_DESC_F_NEXT))
            break;
        i = desc->next;
        if (i >= v->num)
            return -1;
    }

    v->last_avail++;
    g_pending.valid = 1;
    return (long)got;
}

/* Publish the completed chain and poke the guest. */
static int push_used(struct vu_dev *d, uint32_t written)
{
    struct vu_vring *v = &d->vq;
    uint16_t idx = v->used->idx;
    uint64_t one = 1;

    v->used->ring[idx % v->num].id = g_pending.head;
    v->used->ring[idx % v->num].len = written;
    /* The guest reads idx with acquire, so the entry lands first. */
    __atomic_store_n(&v->used->idx, (uint16_t)(idx + 1), __ATOMIC_RELEASE);
    g_pending.valid = 0;

    if (v->call_fd >= 0 &&
        write(v->call_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
        return -1;
    return 0;
}

/* --------------------------------------------------------- public API */

int vu_listen(struct vu_dev *d, const char *socket_path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };

    memset(d, 0, sizeof(*d));
    d->listen_fd = -1;
    d->conn_fd = -1;
    d->vq.kick_fd = -1;
    d->vq.call_fd = -1;

    if (strlen(socket_path) >= sizeof(sa.sun_path))
        return -1;
    strncpy(sa.sun_path, socket_path, sizeof(sa.sun_path) - 1);
    d->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->listen_fd < 0)
        return -1;
    unlink(socket_path);
    if (bind(d->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(d->listen_fd, 1) < 0) {
        perror("vhost-user: bind/listen");
        close(d->listen_fd);
        d->listen_fd = -1;
        return -1;
    }
    return 0;
}

int vu_recv(struct vu_dev *d, struct rproto_hdr *hdr, uint8_t *payload,
            uint32_t cap)
{
    uint8_t buf[RPROTO_HDR_SIZE + RPROTO_MAX_PAYLOAD];

    for (;;) {
        struct pollfd pfds[3];
        int n = 0, kick_slot = -1, listen_slot = -1;

        if (d->conn_fd < 0) {
            /* Wait for QEMU to attach. */
            int c = accept(d->listen_fd, NULL, NULL);

            if (c < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            d->conn_fd = c;
            fprintf(stderr, "vhost-user: frontend connected\n");
            continue;
        }

        /* A chain may already be queued from a previous kick. */
        if (d->running) {
            long got = pop_chain(d, buf, sizeof(buf));

            if (got >= 0) {
                if (got < RPROTO_HDR_SIZE ||
                    rproto_decode_hdr(buf, (size_t)got, hdr) < 0)
                    return -1;
                if (hdr->payload_len > cap ||
                    RPROTO_HDR_SIZE + hdr->payload_len > (uint32_t)got)
                    return -1;
                memcpy(payload, buf + RPROTO_HDR_SIZE, hdr->payload_len);
                return 0;
            }
        }

        pfds[n].fd = d->conn_fd;
        pfds[n].events = POLLIN;
        n++;
        if (d->vq.kick_fd >= 0) {
            kick_slot = n;
            pfds[n].fd = d->vq.kick_fd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (d->listen_fd >= 0) {
            listen_slot = n;
            pfds[n].fd = d->listen_fd;
            pfds[n].events = POLLIN;
            n++;
        }

        if (poll(pfds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            int r = handle_message(d);

            if (r == 1) {
                fprintf(stderr, "vhost-user: frontend disconnected\n");
                close(d->conn_fd);
                d->conn_fd = -1;
                /* Everything the frontend told us dies with the
                 * connection; the next one starts from scratch. */
                unmap_regions(d);
                if (d->vq.kick_fd >= 0)
                    close(d->vq.kick_fd);
                if (d->vq.call_fd >= 0)
                    close(d->vq.call_fd);
                memset(&d->vq, 0, sizeof(d->vq));
                d->vq.kick_fd = -1;
                d->vq.call_fd = -1;
                return 1;
            }
            if (r < 0)
                return -1;
            continue;
        }
        if (kick_slot >= 0 && (pfds[kick_slot].revents & POLLIN)) {
            uint64_t cnt;

            if (read(d->vq.kick_fd, &cnt, sizeof(cnt)) !=
                (ssize_t)sizeof(cnt))
                return -1;
            continue; /* loop round and drain the queue */
        }
        if (listen_slot >= 0 && (pfds[listen_slot].revents & POLLIN)) {
            /* A second frontend is not supported; refuse politely. */
            int c = accept(d->listen_fd, NULL, NULL);

            if (c >= 0)
                close(c);
        }
    }
}

int vu_send_reply(struct vu_dev *d, uint32_t type, uint32_t seq,
                  const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hbuf[RPROTO_HDR_SIZE];
    struct rproto_hdr h = { .type = type, .seq = seq,
                            .payload_len = payload_len };
    uint32_t total = RPROTO_HDR_SIZE + payload_len;

    if (!g_pending.valid || !g_pending.resp)
        return -1;
    if (total > g_pending.resp_cap)
        return -1;
    rproto_encode_hdr(hbuf, &h);
    memcpy(g_pending.resp, hbuf, RPROTO_HDR_SIZE);
    if (payload_len)
        memcpy(g_pending.resp + RPROTO_HDR_SIZE, payload, payload_len);
    return push_used(d, total);
}

void vu_close(struct vu_dev *d)
{
    unmap_regions(d);
    if (d->vq.kick_fd >= 0)
        close(d->vq.kick_fd);
    if (d->vq.call_fd >= 0)
        close(d->vq.call_fd);
    if (d->conn_fd >= 0)
        close(d->conn_fd);
    if (d->listen_fd >= 0)
        close(d->listen_fd);
    d->vq.kick_fd = d->vq.call_fd = d->conn_fd = d->listen_fd = -1;
}
