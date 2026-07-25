/*
 * render_client.h - guest-side client library for renderer protocol v0.
 *
 * The connect functions are the only transport-aware code; everything else
 * speaks the protocol over an already-connected stream fd, shared memory
 * ring, or doorbell-driven ring, so later phases can swap the transport
 * without touching callers.
 */
#ifndef RENDER_CLIENT_H
#define RENDER_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "rproto_shm.h"

/* Doorbell notification state (Phase 3), used when db.fd >= 0. */
struct rc_doorbell {
    int fd;             /* /dev/ivshmem-rproto */
    uint32_t ring_value; /* (host peer id << 16) | vector */
};

struct render_client {
    int fd;          /* stream transports; -1 otherwise */
    int use_shm;
    struct rshm shm;
    void *shm_base;
    size_t shm_size;
    struct rc_doorbell db;
    struct rshm_notifier notifier;
    const struct rshm_notifier *notifier_p; /* NULL = poll the ring */
    uint32_t next_seq;
};

/* All functions return 0 on success, -1 on failure. */
int rc_connect_unix(struct render_client *rc, const char *path);
int rc_connect_vsock(struct render_client *rc, uint32_t cid, uint32_t port);
/* addr is a numeric IPv4 address. Dev/CI transport for hosts without
 * vhost-vsock; from a guest on user-mode networking use 10.0.2.2. */
int rc_connect_tcp(struct render_client *rc, const char *addr, uint16_t port);
/* Phase 2 shared-memory transport, polled. _file maps a file also mapped
 * by renderd --shm (host-side testing). _pci finds the ivshmem-plain
 * device (1af4:1110) on the PCI bus and maps its BAR2 via sysfs
 * resource2 — userspace driver, no kernel module; requires root. */
int rc_connect_shm_file(struct render_client *rc, const char *path);
int rc_connect_shm_pci(struct render_client *rc);
/* Phase 3 doorbell transport: same rings, but through the ivshmem_rproto
 * kernel module, with interrupts instead of polling. Requires the module
 * to be loaded and root to open its character device. */
int rc_connect_doorbell(struct render_client *rc);

/*
 * Attach to a shared region somebody else already mapped, with an
 * optional notifier. Lets a caller supply its own ivshmem plumbing
 * instead of the kernel module — used by the host-side test peer to
 * exercise the doorbell path without a VM. The mapping stays owned by
 * the caller.
 */
int rc_attach_shm(struct render_client *rc, void *base, size_t size,
                  const struct rshm_notifier *n);

/*
 * Parse one transport spec from argv[0..argc-1] and connect. Recognises
 *   --unix PATH | --vsock CID PORT | --tcp ADDR PORT
 *   --shm-file PATH | --shm-pci | --doorbell
 * Returns the number of arguments consumed, or -1 on an unknown or
 * incomplete spec (after printing the accepted forms).
 */
int rc_connect_argv(struct render_client *rc, int argc, char **argv);
const char *rc_transport_usage(void);

/* HELLO/HELLO_ACK version negotiation. Must be the first call. */
int rc_hello(struct render_client *rc);

int rc_create_surface(struct render_client *rc, uint32_t width, uint32_t height);
int rc_clear(struct render_client *rc, uint32_t rgba);
int rc_fill_rect(struct render_client *rc, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t rgba);
int rc_present(struct render_client *rc, uint32_t frame_id);
int rc_goodbye(struct render_client *rc);

/*
 * Draw the project's standard test scene and present it as frame_id:
 * a 320x240 surface cleared to 0x102030, a red rectangle at (40,40) 80x60
 * and a green one at (160,120) 100x80. tests/e2e.sh and tests/vm-e2e.sh
 * check those exact pixels, so every caller draws the same thing.
 */
int rc_draw_demo_scene(struct render_client *rc, uint32_t frame_id);

void rc_close(struct render_client *rc);

#endif /* RENDER_CLIENT_H */
