/*
 * render_client.h - guest-side client library for renderer protocol v0.
 *
 * The connect functions are the only transport-aware code; everything else
 * speaks the protocol over an already-connected stream fd, so later phases
 * can swap the transport without touching callers.
 */
#ifndef RENDER_CLIENT_H
#define RENDER_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "rproto_shm.h"

struct render_client {
    int fd;          /* stream transports; -1 for shm */
    int use_shm;
    struct rshm shm;
    void *shm_base;
    size_t shm_size;
    uint32_t next_seq;
};

/* All functions return 0 on success, -1 on failure. */
int rc_connect_unix(struct render_client *rc, const char *path);
int rc_connect_vsock(struct render_client *rc, uint32_t cid, uint32_t port);
/* addr is a numeric IPv4 address. Dev/CI transport for hosts without
 * vhost-vsock; from a guest on user-mode networking use 10.0.2.2. */
int rc_connect_tcp(struct render_client *rc, const char *addr, uint16_t port);
/* Phase 2 shared-memory transport. _file maps a file also mapped by
 * renderd --shm (host-side testing). _pci finds the ivshmem-plain device
 * (1af4:1110) on the PCI bus and maps its BAR2 via sysfs resource2 —
 * userspace driver, no kernel module; requires root in the guest. */
int rc_connect_shm_file(struct render_client *rc, const char *path);
int rc_connect_shm_pci(struct render_client *rc);

/* HELLO/HELLO_ACK version negotiation. Must be the first call. */
int rc_hello(struct render_client *rc);

int rc_create_surface(struct render_client *rc, uint32_t width, uint32_t height);
int rc_clear(struct render_client *rc, uint32_t rgba);
int rc_fill_rect(struct render_client *rc, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t rgba);
int rc_present(struct render_client *rc, uint32_t frame_id);
int rc_goodbye(struct render_client *rc);

void rc_close(struct render_client *rc);

#endif /* RENDER_CLIENT_H */
