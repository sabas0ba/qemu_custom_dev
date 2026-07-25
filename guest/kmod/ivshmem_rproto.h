/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ivshmem_rproto.h - userspace interface of the guest ivshmem driver.
 *
 * Shared verbatim between the kernel module and guest userspace.
 *
 * The character device /dev/ivshmem-rproto offers:
 *   mmap()   the shared memory (BAR2) at offset 0
 *   read()   blocks until the next doorbell interrupt, yields a __u32
 *            event counter (like UIO); poll()/select() work too
 *   ioctl()  ring a peer's doorbell, or query our own peer ID and the
 *            size of the shared region
 */
#ifndef IVSHMEM_RPROTO_H
#define IVSHMEM_RPROTO_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IVSHM_RPROTO_DEVNAME "ivshmem-rproto"
#define IVSHM_RPROTO_DEVPATH "/dev/" IVSHM_RPROTO_DEVNAME

#define IVSHM_IOC_MAGIC 'V'

/*
 * Ring a doorbell. The argument is (peer_id << 16) | vector, the value
 * the ivshmem Doorbell register expects.
 */
#define IVSHM_IOC_RING     _IOW(IVSHM_IOC_MAGIC, 1, __u32)
/* Our own peer ID, as reported by the device's IVPosition register. */
#define IVSHM_IOC_GET_ID   _IOR(IVSHM_IOC_MAGIC, 2, __u32)
/* Size in bytes of the mmap-able shared memory region. */
#define IVSHM_IOC_SHM_SIZE _IOR(IVSHM_IOC_MAGIC, 3, __u64)

#endif /* IVSHMEM_RPROTO_H */
