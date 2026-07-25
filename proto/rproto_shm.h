/*
 * rproto_shm.h - shared-memory transport for renderer protocol v0 (Phase 2).
 *
 * Carries the exact same framed messages as the stream transport
 * (rproto_io.c) through two single-producer/single-consumer byte rings in
 * a shared memory region: g2h (guest->host requests) and h2g
 * (host->guest replies). Notification is by polling; Phase 3 adds
 * interrupts (ivshmem-doorbell).
 *
 * The region is QEMU's ivshmem-plain BAR2 as seen from the guest, and a
 * mmap'ed memory-backend-file on the host. Any shared mapping works —
 * tests use a plain file mapped by two processes.
 *
 * Concurrency model (modeled on virtio's vring index+barrier scheme):
 * prod/cons are free-running uint32 counters (ring sizes are powers of
 * two). The producer copies payload bytes into the ring FIRST, then
 * publishes them with a release store to prod; the consumer reads prod
 * with an acquire load, so the payload bytes are visible before the
 * index. cons is updated release / read acquire symmetrically for space
 * reuse. Host and guest share physical memory, but compiler/CPU
 * reordering behaves exactly as between two SMP threads, hence the
 * barriers are mandatory.
 *
 * Layout (all offsets from the start of the region):
 *   0x0000  struct rshm_hdr   (magic published LAST with a release store)
 *   g2h_off struct rshm_ring_hdr + g2h_size data bytes
 *   h2g_off struct rshm_ring_hdr + h2g_size data bytes
 *   fb_off  reserved for a future guest-written framebuffer (unused in
 *           Phase 2; commands still drive rendering on the host)
 */
#ifndef RPROTO_SHM_H
#define RPROTO_SHM_H

#include <stddef.h>
#include <stdint.h>

#include "rproto.h"

#define RSHM_MAGIC        0x30534456u /* "VDS0" in little-endian byte order */
#define RSHM_VERSION      1u

/* Default region layout: 4 MiB total (ivshmem BAR sizes must be powers of
 * two), two 256 KiB rings, remainder reserved for a future framebuffer. */
#define RSHM_DEFAULT_SIZE (4u << 20)
#define RSHM_RING_SIZE    (256u << 10)

struct rshm_hdr {
    uint32_t magic;      /* written last by rshm_init (release) */
    uint32_t version;
    uint32_t total_size;
    uint32_t g2h_off;
    uint32_t g2h_size;
    uint32_t h2g_off;
    uint32_t h2g_size;
    uint32_t fb_off;
    uint32_t fb_size;
};

/* prod and cons live on separate cache lines to avoid false sharing.
 * Ring data bytes follow this header directly. */
struct rshm_ring_hdr {
    uint32_t prod;
    uint8_t _pad0[60];
    uint32_t cons;
    uint8_t _pad1[60];
};

struct rshm_ring {
    struct rshm_ring_hdr *hdr;
    uint8_t *data;
    uint32_t size; /* power of two */
};

struct rshm {
    struct rshm_hdr *hdr;
    struct rshm_ring g2h;
    struct rshm_ring h2g;
};

/*
 * rshm_init: lay out and publish the region (host side). base/size is the
 * whole shared mapping. Returns 0, or -1 if size is too small.
 * rshm_attach: validate a region published by rshm_init (guest side),
 * polling until the magic appears or timeout_ms expires (0 = single
 * check, negative = wait forever). Returns 0 or -1.
 * rshm_reset_rings: reset both rings to empty (host side, between
 * sessions; callers must ensure no peer is mid-operation).
 */
int rshm_init(struct rshm *s, void *base, size_t size);
int rshm_attach(struct rshm *s, void *base, size_t size, int timeout_ms);
void rshm_reset_rings(struct rshm *s);

/*
 * Message transfer. A message (12-byte header + payload, same wire format
 * as the stream transport) is published atomically: it is either fully in
 * the ring or not there at all.
 *
 * try variants: 0 = done, 1 = would block (no space / no data), -1 = error.
 * Blocking variants poll; timeout_ms negative = wait forever, else return
 * 1 on timeout. Returns follow the try variants otherwise.
 */
int rshm_msg_try_send(struct rshm_ring *r, uint32_t type, uint32_t seq,
                      const uint8_t *payload, uint32_t payload_len);
int rshm_msg_try_recv(struct rshm_ring *r, struct rproto_hdr *hdr,
                      uint8_t *payload, uint32_t cap);
int rshm_msg_send(struct rshm_ring *r, uint32_t type, uint32_t seq,
                  const uint8_t *payload, uint32_t payload_len,
                  int timeout_ms);
int rshm_msg_recv(struct rshm_ring *r, struct rproto_hdr *hdr,
                  uint8_t *payload, uint32_t cap, int timeout_ms);

#endif /* RPROTO_SHM_H */
