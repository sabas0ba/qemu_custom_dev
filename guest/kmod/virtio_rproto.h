/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * virtio_rproto.h - userspace interface of the guest virtio driver.
 *
 * Shared verbatim between the kernel module and guest userspace.
 *
 * The character device /dev/virtio-rproto offers a single ioctl. The
 * renderer protocol is strictly request/response, and so is a virtqueue
 * chain: one read-only descriptor carrying the request, one write-only
 * descriptor for the reply. One ioctl is one chain.
 */
#ifndef VIRTIO_RPROTO_H
#define VIRTIO_RPROTO_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VIRTIO_RPROTO_DEVNAME "virtio-rproto"
#define VIRTIO_RPROTO_DEVPATH "/dev/" VIRTIO_RPROTO_DEVNAME

/*
 * Largest message either direction, header included. The driver keeps
 * one bounce buffer of this size per direction, so it bounds what a
 * single ioctl can move; it is deliberately larger than the protocol's
 * own limit (RPROTO_HDR_SIZE + RPROTO_MAX_PAYLOAD) so the two can be
 * revised independently.
 */
#define VIRTIO_RPROTO_MSG_MAX 8192u

struct virtio_rproto_xfer {
	__u64 request;		/* pointer to the request bytes */
	__u64 response;		/* pointer to the reply buffer */
	__u32 request_len;	/* bytes to send, <= VIRTIO_RPROTO_MSG_MAX */
	__u32 response_cap;	/* room in the reply buffer */
	__u32 response_len;	/* out: bytes the host wrote */
	__u32 pad;		/* must be zero */
};

#define VIRTIO_RPROTO_IOC_MAGIC 'W'

/* Send one request and block until the host completes the chain. */
#define VIRTIO_RPROTO_IOC_XFER \
	_IOWR(VIRTIO_RPROTO_IOC_MAGIC, 1, struct virtio_rproto_xfer)
/* Largest message this driver will carry, in bytes. */
#define VIRTIO_RPROTO_IOC_MSG_MAX \
	_IOR(VIRTIO_RPROTO_IOC_MAGIC, 2, __u32)

#endif /* VIRTIO_RPROTO_H */
