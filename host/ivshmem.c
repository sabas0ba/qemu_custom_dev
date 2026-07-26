/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ivshmem.h"

int ivshmem_msg_send(int sock, int64_t value, int fd)
{
    /* The value is always little-endian on the wire; hosts we target are
     * LE, but encode explicitly so the format does not depend on it. */
    uint8_t buf[8];
    uint64_t v = (uint64_t)value;
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_u;

    for (int i = 0; i < 8; i++)
        buf[i] = (uint8_t)((v >> (8 * i)) & 0xff);

    if (fd >= 0) {
        struct cmsghdr *cmsg;

        memset(&cmsg_u, 0, sizeof(cmsg_u));
        msg.msg_control = cmsg_u.buf;
        msg.msg_controllen = sizeof(cmsg_u.buf);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }

    for (;;) {
        ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        return n == (ssize_t)sizeof(buf) ? 0 : -1;
    }
}

int ivshmem_msg_recv(int sock, int64_t *value, int *fd)
{
    uint8_t buf[8];
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg_u;
    struct cmsghdr *cmsg;
    ssize_t n;
    uint64_t v = 0;

    *fd = -1;
    memset(&cmsg_u, 0, sizeof(cmsg_u));
    msg.msg_control = cmsg_u.buf;
    msg.msg_controllen = sizeof(cmsg_u.buf);

    do {
        n = recvmsg(sock, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n == 0)
        return 1;
    if (n != (ssize_t)sizeof(buf))
        return -1;

    for (int i = 7; i >= 0; i--)
        v = (v << 8) | buf[i];
    *value = (int64_t)v;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
    }
    return 0;
}

static struct ivshmem_peer *peer_find(struct ivshmem_client *c, int64_t id)
{
    for (int i = 0; i < c->npeers; i++)
        if (c->peers[i].id == id)
            return &c->peers[i];
    return NULL;
}

static struct ivshmem_peer *peer_add(struct ivshmem_client *c, int64_t id)
{
    struct ivshmem_peer *p = peer_find(c, id);

    if (p)
        return p;
    if (c->npeers == (int)(sizeof(c->peers) / sizeof(c->peers[0])))
        return NULL;
    p = &c->peers[c->npeers++];
    p->id = id;
    p->nvectors = 0;
    return p;
}

static void peer_remove(struct ivshmem_client *c, int64_t id)
{
    for (int i = 0; i < c->npeers; i++) {
        if (c->peers[i].id != id)
            continue;
        for (int v = 0; v < c->peers[i].nvectors; v++)
            close(c->peers[i].eventfds[v]);
        c->peers[i] = c->peers[--c->npeers];
        return;
    }
}

/* Handle one ID+fd message. Returns 0, or -1 on protocol violation. */
static int handle_peer_msg(struct ivshmem_client *c, int64_t id, int fd)
{
    struct ivshmem_peer *p;

    if (fd < 0) {
        /* peer left */
        peer_remove(c, id);
        return 0;
    }
    if (id == c->id) {
        if (c->nvectors == IVSHMEM_MAX_VECTORS) {
            close(fd);
            return -1;
        }
        c->eventfds[c->nvectors++] = fd;
        return 0;
    }
    p = peer_add(c, id);
    if (!p || p->nvectors == IVSHMEM_MAX_VECTORS) {
        close(fd);
        return -1;
    }
    p->eventfds[p->nvectors++] = fd;
    return 0;
}

int ivshmem_client_connect(struct ivshmem_client *c, const char *socket_path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int64_t value;
    int fd;

    memset(c, 0, sizeof(*c));
    c->sock = -1;
    c->shm_fd = -1;

    if (strlen(socket_path) >= sizeof(sa.sun_path))
        return -1;
    strncpy(sa.sun_path, socket_path, sizeof(sa.sun_path) - 1);
    c->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->sock < 0)
        return -1;
    if (connect(c->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto fail;

    /* 1. protocol version */
    if (ivshmem_msg_recv(c->sock, &value, &fd) != 0 || fd >= 0)
        goto fail;
    if (value != IVSHMEM_PROTOCOL_VERSION) {
        fprintf(stderr, "ivshmem: unsupported protocol version %lld\n",
                (long long)value);
        goto fail;
    }
    /* 2. our own peer ID */
    if (ivshmem_msg_recv(c->sock, &value, &fd) != 0 || fd >= 0)
        goto fail;
    c->id = value;
    /* 3. shared memory fd */
    if (ivshmem_msg_recv(c->sock, &value, &fd) != 0 || value != -1 || fd < 0)
        goto fail;
    c->shm_fd = fd;

    /*
     * 4/5. Peers, then our own eventfds. Our own arrive last, so the
     * handshake ends once we hold at least one of them and the socket has
     * nothing more pending.
     */
    while (c->nvectors == 0) {
        if (ivshmem_msg_recv(c->sock, &value, &fd) != 0)
            goto fail;
        if (handle_peer_msg(c, value, fd) < 0)
            goto fail;
    }
    /* drain any remaining vectors of our own that are already queued */
    for (;;) {
        struct pollfd pfd = { .fd = c->sock, .events = POLLIN };
        int r = poll(&pfd, 1, 0);

        if (r <= 0)
            break;
        if (ivshmem_msg_recv(c->sock, &value, &fd) != 0)
            goto fail;
        if (handle_peer_msg(c, value, fd) < 0)
            goto fail;
    }
    return 0;

fail:
    ivshmem_client_close(c);
    return -1;
}

int ivshmem_client_poll(struct ivshmem_client *c, int timeout_ms)
{
    struct pollfd pfd = { .fd = c->sock, .events = POLLIN };
    int64_t value;
    int fd, r;

    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    if (r == 0)
        return 1;
    if (ivshmem_msg_recv(c->sock, &value, &fd) != 0)
        return -1; /* server went away */
    return handle_peer_msg(c, value, fd);
}

int ivshmem_client_wait_peer(struct ivshmem_client *c, int timeout_ms)
{
    int waited = 0;

    while (c->npeers == 0) {
        int step = timeout_ms < 0 ? -1 : 50;
        int r = ivshmem_client_poll(c, step);

        if (r < 0)
            return -1;
        if (r == 1 && timeout_ms >= 0) {
            waited += step;
            if (waited >= timeout_ms)
                return -1;
        }
    }
    return 0;
}

int ivshmem_client_notify(struct ivshmem_client *c, int64_t peer_id,
                          int vector)
{
    struct ivshmem_peer *p = peer_find(c, peer_id);
    uint64_t one = 1;

    if (!p || vector >= p->nvectors)
        return -1;
    for (;;) {
        ssize_t n = write(p->eventfds[vector], &one, sizeof(one));

        if (n < 0 && errno == EINTR)
            continue;
        return n == (ssize_t)sizeof(one) ? 0 : -1;
    }
}

int ivshmem_client_wait_irq(struct ivshmem_client *c, int vector,
                            int timeout_ms)
{
    struct pollfd pfds[2];
    uint64_t cnt;
    int r;

    if (vector >= c->nvectors)
        return -1;
    pfds[0].fd = c->eventfds[vector];
    pfds[0].events = POLLIN;
    /* Watch the server too: peers come and go while we wait, and their
     * eventfds arrive on this socket. */
    pfds[1].fd = c->sock;
    pfds[1].events = POLLIN;
    do {
        r = poll(pfds, 2, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    if (r == 0)
        return 1;
    if (pfds[1].revents & POLLIN) {
        int64_t value;
        int fd;

        if (ivshmem_msg_recv(c->sock, &value, &fd) != 0)
            return -1;
        if (handle_peer_msg(c, value, fd) < 0)
            return -1;
    }
    if (!(pfds[0].revents & POLLIN))
        return 1; /* only server traffic; callers re-check and wait again */
    if (read(pfds[0].fd, &cnt, sizeof(cnt)) != (ssize_t)sizeof(cnt))
        return -1;
    return 0;
}

int64_t ivshmem_client_first_peer(const struct ivshmem_client *c)
{
    return c->npeers > 0 ? c->peers[0].id : IVSHMEM_NO_PEER;
}

void ivshmem_client_close(struct ivshmem_client *c)
{
    for (int i = 0; i < c->nvectors; i++)
        close(c->eventfds[i]);
    c->nvectors = 0;
    for (int i = 0; i < c->npeers; i++)
        for (int v = 0; v < c->peers[i].nvectors; v++)
            close(c->peers[i].eventfds[v]);
    c->npeers = 0;
    if (c->shm_fd >= 0) {
        close(c->shm_fd);
        c->shm_fd = -1;
    }
    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
}
