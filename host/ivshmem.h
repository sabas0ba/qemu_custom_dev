/*
 * ivshmem.h - ivshmem server/client protocol (Phase 3).
 *
 * Implements the socket protocol QEMU's ivshmem-doorbell device speaks,
 * as specified in the QEMU tree (docs/specs/ivshmem-spec.rst). We provide
 * both ends ourselves: `ivshmemd` is the server, and renderd is a peer —
 * QEMU is another peer. QEMU itself stays unmodified.
 *
 * Wire format: every message is one 64-bit little-endian integer, with at
 * most one file descriptor attached via SCM_RIGHTS.
 *
 * Handshake, server -> new client:
 *   1. protocol version (0)
 *   2. the client's own peer ID
 *   3. -1, with the shared memory fd attached
 *   4. for every peer that already exists: its ID once per vector, each
 *      with an eventfd attached — writing to that eventfd raises the
 *      corresponding interrupt vector on that peer
 *   5. the client's own ID once per vector, each with the eventfd the
 *      client itself must wait on. Sent last, so receiving them marks the
 *      end of the handshake.
 *
 * Afterwards the socket stays open: a peer joining produces the same kind
 * of ID+eventfd messages, and a peer leaving produces its ID with no fd
 * attached.
 */
#ifndef IVSHMEM_H
#define IVSHMEM_H

#include <stdint.h>

#define IVSHMEM_PROTOCOL_VERSION 0
#define IVSHMEM_MAX_VECTORS      8
/* A peer ID that is not a valid one, used for "none". */
#define IVSHMEM_NO_PEER          (-1)

/*
 * Message I/O. fd is passed/returned as -1 when no descriptor is attached.
 * send returns 0 or -1; recv returns 0, 1 on orderly EOF, or -1.
 */
int ivshmem_msg_send(int sock, int64_t value, int fd);
int ivshmem_msg_recv(int sock, int64_t *value, int *fd);

/* One remote peer as seen by a client. */
struct ivshmem_peer {
    int64_t id;
    int nvectors;
    int eventfds[IVSHMEM_MAX_VECTORS]; /* write here to interrupt the peer */
};

struct ivshmem_client {
    int sock;
    int64_t id;
    int shm_fd;
    int nvectors;                          /* vectors on ourselves */
    int eventfds[IVSHMEM_MAX_VECTORS];     /* read here for our interrupts */
    struct ivshmem_peer peers[16];
    int npeers;
};

/*
 * Connect to an ivshmem server and run the handshake. On success the
 * client owns shm_fd and the eventfds. Returns 0 or -1.
 */
int ivshmem_client_connect(struct ivshmem_client *c, const char *socket_path);

/*
 * Process one pending server message (peer join/leave). Returns 0 when a
 * message was handled, 1 when none was pending within timeout_ms
 * (negative waits forever), -1 on error or server shutdown.
 */
int ivshmem_client_poll(struct ivshmem_client *c, int timeout_ms);

/* Block until at least one peer other than us exists. 0 or -1. */
int ivshmem_client_wait_peer(struct ivshmem_client *c, int timeout_ms);

/* Raise vector `vector` on peer `peer_id`. Returns 0 or -1. */
int ivshmem_client_notify(struct ivshmem_client *c, int64_t peer_id,
                          int vector);

/*
 * Wait for one of our own interrupts on `vector`. Returns 0 when
 * signalled, 1 on timeout, -1 on error. timeout_ms negative waits
 * forever.
 */
int ivshmem_client_wait_irq(struct ivshmem_client *c, int vector,
                            int timeout_ms);

/* ID of some peer other than us, or IVSHMEM_NO_PEER when alone. */
int64_t ivshmem_client_first_peer(const struct ivshmem_client *c);

void ivshmem_client_close(struct ivshmem_client *c);

#endif /* IVSHMEM_H */
