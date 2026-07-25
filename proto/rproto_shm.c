#include <string.h>
#include <time.h>

#include "rproto_shm.h"

#define POLL_INTERVAL_NS (50 * 1000) /* 50us between polls */

static void poll_pause(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_INTERVAL_NS };

    nanosleep(&ts, NULL);
}

/* timeout bookkeeping: number of polls left, or -1 for forever */
static long timeout_polls(int timeout_ms)
{
    if (timeout_ms < 0)
        return -1;
    return (long)timeout_ms * 1000000L / POLL_INTERVAL_NS + 1;
}

static void ring_setup(struct rshm_ring *r, uint8_t *base, uint32_t off,
                       uint32_t size)
{
    r->hdr = (struct rshm_ring_hdr *)(base + off);
    r->data = base + off + sizeof(struct rshm_ring_hdr);
    r->size = size;
}

int rshm_init(struct rshm *s, void *base, size_t size, int32_t host_peer_id)
{
    uint8_t *b = base;
    uint32_t ring_span = (uint32_t)sizeof(struct rshm_ring_hdr) + RSHM_RING_SIZE;
    uint32_t g2h_off = (uint32_t)sizeof(struct rshm_hdr);
    /* keep ring headers cache-line aligned */
    g2h_off = (g2h_off + 63u) & ~63u;
    uint32_t h2g_off = g2h_off + ring_span;
    uint32_t fb_off = h2g_off + ring_span;

    if (size < fb_off || size > UINT32_MAX)
        return -1;

    s->hdr = (struct rshm_hdr *)b;
    memset(b, 0, fb_off);
    s->hdr->version = RSHM_VERSION;
    s->hdr->total_size = (uint32_t)size;
    s->hdr->g2h_off = g2h_off;
    s->hdr->g2h_size = RSHM_RING_SIZE;
    s->hdr->h2g_off = h2g_off;
    s->hdr->h2g_size = RSHM_RING_SIZE;
    s->hdr->fb_off = fb_off;
    s->hdr->fb_size = (uint32_t)size - fb_off;
    s->hdr->host_peer_id = host_peer_id;
    ring_setup(&s->g2h, b, g2h_off, RSHM_RING_SIZE);
    ring_setup(&s->h2g, b, h2g_off, RSHM_RING_SIZE);

    /* Publish: everything above must be visible before the magic. */
    __atomic_store_n(&s->hdr->magic, RSHM_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

int rshm_attach(struct rshm *s, void *base, size_t size, int timeout_ms)
{
    uint8_t *b = base;
    struct rshm_hdr *hdr = (struct rshm_hdr *)b;
    long polls = timeout_polls(timeout_ms);

    if (size < sizeof(*hdr))
        return -1;
    while (__atomic_load_n(&hdr->magic, __ATOMIC_ACQUIRE) != RSHM_MAGIC) {
        if (polls == 0)
            return -1;
        if (polls > 0)
            polls--;
        poll_pause();
    }
    if (hdr->version != RSHM_VERSION || hdr->total_size > size)
        return -1;
    /* ring sizes must be powers of two and inside the region */
    if (hdr->g2h_size == 0 || (hdr->g2h_size & (hdr->g2h_size - 1)) ||
        hdr->h2g_size == 0 || (hdr->h2g_size & (hdr->h2g_size - 1)))
        return -1;
    if ((uint64_t)hdr->g2h_off + sizeof(struct rshm_ring_hdr) + hdr->g2h_size >
            hdr->total_size ||
        (uint64_t)hdr->h2g_off + sizeof(struct rshm_ring_hdr) + hdr->h2g_size >
            hdr->total_size)
        return -1;

    s->hdr = hdr;
    ring_setup(&s->g2h, b, hdr->g2h_off, hdr->g2h_size);
    ring_setup(&s->h2g, b, hdr->h2g_off, hdr->h2g_size);
    return 0;
}

void rshm_reset_rings(struct rshm *s)
{
    __atomic_store_n(&s->g2h.hdr->prod, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->g2h.hdr->cons, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->h2g.hdr->prod, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->h2g.hdr->cons, 0, __ATOMIC_RELEASE);
}

/* Copy len bytes into the ring at free-running position pos (may wrap). */
static void ring_copy_in(struct rshm_ring *r, uint32_t pos,
                         const uint8_t *src, uint32_t len)
{
    uint32_t off = pos & (r->size - 1);
    uint32_t first = len < r->size - off ? len : r->size - off;

    memcpy(r->data + off, src, first);
    memcpy(r->data, src + first, len - first);
}

static void ring_copy_out(struct rshm_ring *r, uint32_t pos,
                          uint8_t *dst, uint32_t len)
{
    uint32_t off = pos & (r->size - 1);
    uint32_t first = len < r->size - off ? len : r->size - off;

    memcpy(dst, r->data + off, first);
    memcpy(dst + first, r->data, len - first);
}

int rshm_msg_try_send(struct rshm_ring *r, uint32_t type, uint32_t seq,
                      const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hbuf[RPROTO_HDR_SIZE];
    struct rproto_hdr h = { .type = type, .seq = seq,
                            .payload_len = payload_len };
    uint32_t need = RPROTO_HDR_SIZE + payload_len;
    uint32_t prod, cons;

    if (payload_len > RPROTO_MAX_PAYLOAD || need > r->size)
        return -1;
    prod = r->hdr->prod; /* we are the only producer */
    cons = __atomic_load_n(&r->hdr->cons, __ATOMIC_ACQUIRE);
    if (r->size - (prod - cons) < need)
        return 1;
    rproto_encode_hdr(hbuf, &h);
    ring_copy_in(r, prod, hbuf, RPROTO_HDR_SIZE);
    if (payload_len > 0)
        ring_copy_in(r, prod + RPROTO_HDR_SIZE, payload, payload_len);
    /* publish the whole message at once */
    __atomic_store_n(&r->hdr->prod, prod + need, __ATOMIC_RELEASE);
    return 0;
}

int rshm_msg_try_recv(struct rshm_ring *r, struct rproto_hdr *hdr,
                      uint8_t *payload, uint32_t cap)
{
    uint8_t hbuf[RPROTO_HDR_SIZE];
    uint32_t cons, prod, avail;

    cons = r->hdr->cons; /* we are the only consumer */
    prod = __atomic_load_n(&r->hdr->prod, __ATOMIC_ACQUIRE);
    avail = prod - cons;
    if (avail < RPROTO_HDR_SIZE)
        return 1;
    ring_copy_out(r, cons, hbuf, RPROTO_HDR_SIZE);
    if (rproto_decode_hdr(hbuf, sizeof(hbuf), hdr) < 0)
        return -1;
    if (hdr->payload_len > cap)
        return -1;
    /* the producer publishes messages whole, so the payload is present */
    if (avail < RPROTO_HDR_SIZE + hdr->payload_len)
        return -1;
    if (hdr->payload_len > 0)
        ring_copy_out(r, cons + RPROTO_HDR_SIZE, payload, hdr->payload_len);
    __atomic_store_n(&r->hdr->cons, cons + RPROTO_HDR_SIZE + hdr->payload_len,
                     __ATOMIC_RELEASE);
    return 0;
}

int rshm_msg_send_n(struct rshm_ring *r, uint32_t type, uint32_t seq,
                    const uint8_t *payload, uint32_t payload_len,
                    int timeout_ms, const struct rshm_notifier *n)
{
    long polls = timeout_polls(timeout_ms);

    for (;;) {
        int rc = rshm_msg_try_send(r, type, seq, payload, payload_len);

        if (rc == 0) {
            /* published; only now may the peer be told to look */
            if (n && n->notify && n->notify(n->ctx) < 0)
                return -1;
            return 0;
        }
        if (rc != 1)
            return rc;
        if (polls == 0)
            return 1;
        if (polls > 0)
            polls--;
        poll_pause();
    }
}

int rshm_msg_recv_n(struct rshm_ring *r, struct rproto_hdr *hdr,
                    uint8_t *payload, uint32_t cap, int timeout_ms,
                    const struct rshm_notifier *n)
{
    long polls;
    int left = timeout_ms;

    if (n && n->wait) {
        for (;;) {
            int rc = rshm_msg_try_recv(r, hdr, payload, cap);
            int wr;

            if (rc != 1)
                return rc;
            /* Ring empty: block until the peer rings, in bounded steps so
             * a lost signal cannot wedge us forever. */
            wr = n->wait(n->ctx, left < 0 ? 1000 : (left < 1000 ? left : 1000));
            if (wr < 0)
                return -1;
            if (wr == 1 && left >= 0) {
                left -= left < 1000 ? left : 1000;
                if (left == 0)
                    return 1;
            }
        }
    }

    polls = timeout_polls(timeout_ms);
    for (;;) {
        int rc = rshm_msg_try_recv(r, hdr, payload, cap);

        if (rc != 1)
            return rc;
        if (polls == 0)
            return 1;
        if (polls > 0)
            polls--;
        poll_pause();
    }
}

int rshm_msg_send(struct rshm_ring *r, uint32_t type, uint32_t seq,
                  const uint8_t *payload, uint32_t payload_len,
                  int timeout_ms)
{
    return rshm_msg_send_n(r, type, seq, payload, payload_len, timeout_ms,
                           NULL);
}

int rshm_msg_recv(struct rshm_ring *r, struct rproto_hdr *hdr,
                  uint8_t *payload, uint32_t cap, int timeout_ms)
{
    return rshm_msg_recv_n(r, hdr, payload, cap, timeout_ms, NULL);
}
