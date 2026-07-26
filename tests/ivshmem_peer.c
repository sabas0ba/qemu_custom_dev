/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ivshmem_peer - stands in for the guest in host-only doorbell tests.
 *
 * Joins an ivshmem server as an ordinary peer, maps the shared region,
 * and drives the renderer protocol over it with interrupt notification —
 * exactly what the guest does, except that the eventfds come straight
 * from the server rather than through QEMU and the kernel module. That
 * covers the server protocol, the client handshake, the doorbell wakeups
 * and the ring, with no VM involved; tests/vm-e2e.sh then covers the
 * parts only a real guest has (PCI, MSI-X, the driver).
 *
 * Draws the same scene as guest/user/demo.c.
 *
 * Usage: ivshmem_peer --socket PATH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivshmem.h"
#include "render_client.h"

static int peer_notify(void *ctx)
{
    struct ivshmem_client *cli = ctx;
    int64_t peer = ivshmem_client_first_peer(cli);

    if (peer == IVSHMEM_NO_PEER)
        return -1;
    return ivshmem_client_notify(cli, peer, 0);
}

static int peer_wait(void *ctx, int timeout_ms)
{
    return ivshmem_client_wait_irq(ctx, 0, timeout_ms);
}

int main(int argc, char **argv)
{
    struct ivshmem_client cli;
    struct render_client rc;
    struct rshm_notifier notifier = {
        .notify = peer_notify, .wait = peer_wait, .ctx = &cli,
    };
    struct stat st;
    void *base;
    int rv = 1;

    if (argc != 3 || strcmp(argv[1], "--socket") != 0) {
        fprintf(stderr, "usage: ivshmem_peer --socket PATH\n");
        return 2;
    }

    if (ivshmem_client_connect(&cli, argv[2]) < 0) {
        fprintf(stderr, "ivshmem_peer: cannot join %s\n", argv[2]);
        return 1;
    }
    if (fstat(cli.shm_fd, &st) < 0 || st.st_size <= 0) {
        perror("ivshmem_peer: fstat");
        goto out_client;
    }
    base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
                MAP_SHARED, cli.shm_fd, 0);
    if (base == MAP_FAILED) {
        perror("ivshmem_peer: mmap");
        goto out_client;
    }
    /* The host must already be a peer for its doorbell to be ringable. */
    if (ivshmem_client_wait_peer(&cli, 5000) < 0) {
        fprintf(stderr, "ivshmem_peer: no other peer joined\n");
        goto out_map;
    }
    if (rc_attach_shm(&rc, base, (size_t)st.st_size, &notifier) < 0) {
        fprintf(stderr, "ivshmem_peer: no valid shm layout\n");
        goto out_map;
    }
    fprintf(stderr, "ivshmem_peer: joined as peer %lld, host peer %d\n",
            (long long)cli.id, rc.shm.hdr->host_peer_id);

    if (rc_hello(&rc) < 0 ||
        rc_draw_demo_scene(&rc, 1) < 0 ||
        rc_goodbye(&rc) < 0) {
        fprintf(stderr, "ivshmem_peer: protocol error\n");
        rc_close(&rc);
        goto out_map;
    }
    rc_close(&rc);
    printf("ivshmem_peer: frame 1 presented\n");
    rv = 0;

out_map:
    munmap(base, (size_t)st.st_size);
out_client:
    ivshmem_client_close(&cli);
    return rv;
}
