/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ivshmemd - minimal ivshmem server (Phase 3).
 *
 * Brokers a shared memory region and per-peer eventfds between QEMU's
 * ivshmem-doorbell device and the host renderer daemon, speaking the
 * protocol described in host/ivshmem.h. QEMU is used unmodified; it
 * connects here through `-chardev socket,path=...` plus
 * `-device ivshmem-doorbell,chardev=...`.
 *
 * Ubuntu does not package QEMU's contrib ivshmem-server, and the protocol
 * is small, so we implement it rather than adding a build-from-source
 * dependency.
 *
 * Usage:
 *   ivshmemd --socket PATH --shm FILE [--size BYTES] [--vectors N]
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ivshmem.h"

/*
 * QEMU's ivshmem device rejects peer IDs at or above its own limit of 16,
 * so IDs are handed out from that range and reused once a peer leaves
 * rather than counted upwards forever.
 */
#define MAX_PEERS 16

struct peer {
    int64_t id;
    int sock;
    int eventfds[IVSHMEM_MAX_VECTORS];
};

static struct peer g_peers[MAX_PEERS];
static int g_npeers;
static int g_nvectors = 1;
static int g_shm_fd = -1;

static void peer_close(int idx)
{
    struct peer *p = &g_peers[idx];
    int64_t gone = p->id;

    for (int v = 0; v < g_nvectors; v++)
        close(p->eventfds[v]);
    close(p->sock);
    g_peers[idx] = g_peers[--g_npeers];

    /* tell everyone else: this ID left (ID with no fd attached) */
    for (int i = 0; i < g_npeers; i++)
        ivshmem_msg_send(g_peers[i].sock, gone, -1);
    fprintf(stderr, "ivshmemd: peer %lld left\n", (long long)gone);
}

/* Lowest ID not currently taken, or -1 when all are in use. */
static int64_t alloc_peer_id(void)
{
    for (int64_t id = 0; id < MAX_PEERS; id++) {
        int taken = 0;

        for (int i = 0; i < g_npeers; i++)
            if (g_peers[i].id == id)
                taken = 1;
        if (!taken)
            return id;
    }
    return -1;
}

static int peer_add(int sock)
{
    struct peer *p;
    int64_t id = alloc_peer_id();

    if (g_npeers == MAX_PEERS || id < 0) {
        fprintf(stderr, "ivshmemd: refusing peer, all %d slots in use\n",
                MAX_PEERS);
        close(sock);
        return -1;
    }
    p = &g_peers[g_npeers];
    p->id = id;
    p->sock = sock;
    for (int v = 0; v < g_nvectors; v++) {
        p->eventfds[v] = eventfd(0, 0);
        if (p->eventfds[v] < 0) {
            for (int k = 0; k < v; k++)
                close(p->eventfds[k]);
            close(sock);
            return -1;
        }
    }

    /* 1-3: version, the peer's own ID, and the shared memory fd */
    if (ivshmem_msg_send(sock, IVSHMEM_PROTOCOL_VERSION, -1) < 0 ||
        ivshmem_msg_send(sock, p->id, -1) < 0 ||
        ivshmem_msg_send(sock, -1, g_shm_fd) < 0)
        goto fail;

    /* 4: every peer that already exists, once per vector */
    for (int i = 0; i < g_npeers; i++)
        for (int v = 0; v < g_nvectors; v++)
            if (ivshmem_msg_send(sock, g_peers[i].id,
                                 g_peers[i].eventfds[v]) < 0)
                goto fail;

    /* 5: the peer's own eventfds, last — this ends the handshake */
    for (int v = 0; v < g_nvectors; v++)
        if (ivshmem_msg_send(sock, p->id, p->eventfds[v]) < 0)
            goto fail;

    /* 6: let the existing peers know about the newcomer */
    for (int i = 0; i < g_npeers; i++)
        for (int v = 0; v < g_nvectors; v++)
            ivshmem_msg_send(g_peers[i].sock, p->id, p->eventfds[v]);

    g_npeers++;
    fprintf(stderr, "ivshmemd: peer %lld joined (%d total)\n",
            (long long)p->id, g_npeers);
    return 0;

fail:
    for (int v = 0; v < g_nvectors; v++)
        close(p->eventfds[v]);
    close(sock);
    return -1;
}

static int setup_shm(const char *path, long size)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);

    if (fd < 0) {
        fprintf(stderr, "ivshmemd: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        perror("ivshmemd: ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_unix(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "ivshmemd: socket path too long\n");
        return -1;
    }
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("ivshmemd: socket");
        return -1;
    }
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, MAX_PEERS) < 0) {
        perror("ivshmemd: bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(void)
{
    fprintf(stderr, "usage: ivshmemd --socket PATH --shm FILE"
            " [--size BYTES] [--vectors N]\n");
}

int main(int argc, char **argv)
{
    const char *sock_path = NULL;
    const char *shm_path = NULL;
    long size = 4 << 20;
    int lfd;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            shm_path = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--vectors") == 0 && i + 1 < argc) {
            g_nvectors = (int)strtol(argv[++i], NULL, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (!sock_path || !shm_path || size <= 0 ||
        g_nvectors < 1 || g_nvectors > IVSHMEM_MAX_VECTORS) {
        usage();
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    g_shm_fd = setup_shm(shm_path, size);
    if (g_shm_fd < 0)
        return 1;
    lfd = listen_unix(sock_path);
    if (lfd < 0)
        return 1;
    fprintf(stderr, "ivshmemd: listening on %s (shm %s, %ld bytes,"
            " %d vector(s))\n", sock_path, shm_path, size, g_nvectors);

    for (;;) {
        struct pollfd pfds[MAX_PEERS + 1];
        int n = 0;

        pfds[n].fd = lfd;
        pfds[n].events = POLLIN;
        n++;
        for (int i = 0; i < g_npeers; i++) {
            pfds[n].fd = g_peers[i].sock;
            pfds[n].events = POLLIN;
            n++;
        }
        if (poll(pfds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("ivshmemd: poll");
            break;
        }
        /* Peers only ever close the socket; any readable event on a peer
         * means it is gone. Walk backwards so removal stays safe. */
        for (int i = g_npeers - 1; i >= 0; i--)
            if (pfds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))
                peer_close(i);
        if (pfds[0].revents & POLLIN) {
            int cfd = accept(lfd, NULL, NULL);

            if (cfd >= 0)
                peer_add(cfd);
        }
    }

    close(lfd);
    unlink(sock_path);
    return 0;
}
