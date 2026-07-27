/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vhost_user.h - vhost-user backend for the renderer protocol (Phase 4).
 *
 * QEMU's generic `vhost-user-device-pci` frontend puts a virtio device in
 * the guest whose queues are serviced by a backend process over a unix
 * socket — this one. QEMU stays unmodified; it is the vhost-user
 * frontend, we are the backend, and the guest sees an ordinary virtio
 * device driven by guest/kmod/virtio_rproto.
 *
 * Where the earlier phases put a ring in shared memory and invented the
 * layout, here the layout is virtio's split vring and the memory is the
 * guest's own RAM, handed to us as file descriptors. That is the whole
 * point of the exercise: the same renderer protocol, carried by the
 * mechanism virtio devices actually use.
 *
 * Protocol reference: QEMU docs/interop/vhost-user.rst.
 */
#ifndef VHOST_USER_H
#define VHOST_USER_H

#include <stdint.h>

#include "rproto.h"

/* Requests we implement. Numbering is fixed by the protocol. */
enum vhost_user_request {
    VHOST_USER_GET_FEATURES          = 1,
    VHOST_USER_SET_FEATURES          = 2,
    VHOST_USER_SET_OWNER             = 3,
    VHOST_USER_RESET_OWNER           = 4,
    VHOST_USER_SET_MEM_TABLE         = 5,
    VHOST_USER_SET_LOG_BASE          = 6,
    VHOST_USER_SET_LOG_FD            = 7,
    VHOST_USER_SET_VRING_NUM         = 8,
    VHOST_USER_SET_VRING_ADDR        = 9,
    VHOST_USER_SET_VRING_BASE        = 10,
    VHOST_USER_GET_VRING_BASE        = 11,
    VHOST_USER_SET_VRING_KICK        = 12,
    VHOST_USER_SET_VRING_CALL        = 13,
    VHOST_USER_SET_VRING_ERR         = 14,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM         = 17,
    VHOST_USER_SET_VRING_ENABLE      = 18,
    VHOST_USER_GET_CONFIG            = 24,
    VHOST_USER_SET_CONFIG            = 25,
    VHOST_USER_SET_STATUS            = 39,
    VHOST_USER_GET_STATUS            = 40,
};

#define VHOST_USER_VERSION_MASK   0x3
#define VHOST_USER_REPLY_MASK     (1u << 2)
#define VHOST_USER_NEED_REPLY_MASK (1u << 3)

#define VHOST_USER_MAX_RAM_SLOTS  8
#define VHOST_MEMORY_BASELINE_NREGIONS 8

/* virtio feature bits we care about */
#define VIRTIO_F_VERSION_1        32
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX   29

/* split vring descriptor flags */
#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

struct vhost_user_hdr {
    uint32_t request;
    uint32_t flags;
    uint32_t size;
};

struct vhost_user_mem_region {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
    uint64_t mmap_offset;
};

struct vhost_user_memory {
    uint32_t nregions;
    uint32_t padding;
    struct vhost_user_mem_region regions[VHOST_MEMORY_BASELINE_NREGIONS];
};

struct vhost_user_vring_state {
    uint32_t index;
    uint32_t num;
};

struct vhost_user_vring_addr {
    uint32_t index;
    uint32_t flags;
    uint64_t desc_user_addr;
    uint64_t used_user_addr;
    uint64_t avail_user_addr;
    uint64_t log_guest_addr;
};

/*
 * Split vring, exactly as the virtio spec lays it out in guest memory.
 * Named _raw to keep them distinct from any kernel headers.
 */
struct vring_desc_raw {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail_raw {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem_raw {
    uint32_t id;
    uint32_t len;
};

struct vring_used_raw {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem_raw ring[];
};

/* One mapped slice of guest RAM. */
struct vu_region {
    uint64_t gpa;   /* guest physical base */
    uint64_t qva;   /* QEMU userspace base */
    uint64_t size;
    uint8_t *base;  /* our mapping of it */
    void *mmap_addr;
    size_t mmap_size;
};

struct vu_vring {
    int enabled;
    uint32_t num;
    uint16_t last_avail;
    int kick_fd;
    int call_fd;
    /* Translated into our address space by vu_vring_ready(). */
    struct vring_desc_raw *desc;
    struct vring_avail_raw *avail;
    struct vring_used_raw *used;
    uint64_t desc_qva, avail_qva, used_qva;
};

struct vu_dev {
    int listen_fd;
    int conn_fd;
    uint64_t features;
    struct vu_region regions[VHOST_USER_MAX_RAM_SLOTS];
    int nregions;
    struct vu_vring vq;   /* num_vqs=1: one request/response queue */
    int running;          /* addresses translated and the queue enabled */
};

/*
 * Start listening. Returns 0 or -1. The caller then loops on
 * vu_serve_session(), which blocks until QEMU connects, drives the
 * handshake, and hands requests to the callbacks.
 */
int vu_listen(struct vu_dev *d, const char *socket_path);
void vu_close(struct vu_dev *d);

/*
 * Wait for the next guest request and copy it into payload/hdr, following
 * the rproto_recv convention: 0 on a message, 1 when the connection ended,
 * -1 on error. The reply for it must go out through vu_send_reply()
 * before the next call.
 */
int vu_recv(struct vu_dev *d, struct rproto_hdr *hdr, uint8_t *payload,
            uint32_t cap);
int vu_send_reply(struct vu_dev *d, uint32_t type, uint32_t seq,
                  const uint8_t *payload, uint32_t payload_len);

#endif /* VHOST_USER_H */
