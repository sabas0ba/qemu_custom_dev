// SPDX-License-Identifier: GPL-2.0-only
/*
 * virtio_rproto - guest driver for the Phase 4 vhost-user transport.
 *
 * Phases 2 and 3 put a ring of our own design in a shared memory
 * window. Phase 4 drops that ring and uses virtio's: QEMU's generic
 * `vhost-user-device-pci` frontend creates an ordinary virtio device
 * here, and the queue behind it is serviced by host/renderd running as
 * a vhost-user backend. Nothing in QEMU is modified; the interesting
 * work moved into the backend and into this driver.
 *
 * That means we get the transport for free — virtio_pci does the PCI
 * capability walk, MSI-X setup and vring allocation, and virtio_ring
 * does the descriptor bookkeeping. What is left is exactly the device's
 * own semantics: build a two-descriptor chain (request out, reply in),
 * kick, wait, hand the reply back.
 *
 * Device ID 37 needs a word of explanation. QEMU's generic frontend
 * insists on an ID it has a name for, so an unassigned number is not an
 * option; 37 (VIRTIO_ID_DMABUF) is one of the assigned IDs with no
 * in-tree Linux driver to collide with. This driver does not implement
 * that device or pretend to — it borrows the number so a local
 * experiment can run on an unmodified QEMU. See docs/vhost-user.md.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>

#include "virtio_rproto.h"

#define DRV_NAME "virtio_rproto"

/*
 * How long to wait for the host to complete a chain. A backend that
 * never answers must not leave the caller stuck in D state forever, but
 * the buffers of a timed-out chain still belong to the device, so we
 * cannot reuse them: the device is marked broken and refuses further
 * transfers instead.
 */
#define VRP_XFER_TIMEOUT_MS 30000

struct vrp_dev {
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct miscdevice misc;
	struct mutex lock;	/* serialises the one in-flight chain */
	struct completion done;
	bool broken;
	void *req_buf;
	void *resp_buf;
};

/* Only one device is expected; the misc device needs it by name. */
static struct vrp_dev *vrp_singleton;

static void vrp_vq_callback(struct virtqueue *vq)
{
	struct vrp_dev *d = vq->vdev->priv;

	complete(&d->done);
}

/* Run one request/reply chain. Returns the reply length or a negative errno. */
static long vrp_xfer(struct vrp_dev *d, struct virtio_rproto_xfer *x)
{
	struct scatterlist sg_out, sg_in, *sgs[2];
	unsigned int len = 0;
	void *token;
	long left;
	int err;

	if (x->pad || !x->request_len ||
	    x->request_len > VIRTIO_RPROTO_MSG_MAX ||
	    x->response_cap > VIRTIO_RPROTO_MSG_MAX)
		return -EINVAL;
	if (!x->response_cap)
		return -EINVAL;

	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	if (d->broken) {
		mutex_unlock(&d->lock);
		return -EIO;
	}

	if (copy_from_user(d->req_buf, u64_to_user_ptr(x->request),
			   x->request_len)) {
		mutex_unlock(&d->lock);
		return -EFAULT;
	}

	sg_init_one(&sg_out, d->req_buf, x->request_len);
	sg_init_one(&sg_in, d->resp_buf, x->response_cap);
	sgs[0] = &sg_out;
	sgs[1] = &sg_in;

	reinit_completion(&d->done);
	err = virtqueue_add_sgs(d->vq, sgs, 1, 1, d, GFP_KERNEL);
	if (err) {
		mutex_unlock(&d->lock);
		return err;
	}
	virtqueue_kick(d->vq);

	/*
	 * A signal must not abandon the chain either: the host still owns
	 * the buffers, so treat an interrupted wait the same as a timeout.
	 */
	left = wait_for_completion_killable_timeout(
		&d->done, msecs_to_jiffies(VRP_XFER_TIMEOUT_MS));
	if (left <= 0) {
		d->broken = true;
		dev_err(&d->vdev->dev,
			"host did not complete the request (%s); device disabled\n",
			left == 0 ? "timeout" : "interrupted");
		mutex_unlock(&d->lock);
		return left == 0 ? -ETIMEDOUT : -ERESTARTSYS;
	}

	/* A completion can be signalled slightly ahead of the used entry. */
	while (!(token = virtqueue_get_buf(d->vq, &len)))
		cpu_relax();
	if (token != d) {
		d->broken = true;
		mutex_unlock(&d->lock);
		return -EIO;
	}

	if (len > x->response_cap) {
		mutex_unlock(&d->lock);
		return -EMSGSIZE;
	}
	if (len && copy_to_user(u64_to_user_ptr(x->response), d->resp_buf,
				len)) {
		mutex_unlock(&d->lock);
		return -EFAULT;
	}
	mutex_unlock(&d->lock);

	x->response_len = len;
	return 0;
}

static long vrp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vrp_dev *d = vrp_singleton;
	struct virtio_rproto_xfer x;
	void __user *uarg = (void __user *)arg;
	long ret;

	if (!d)
		return -ENODEV;

	switch (cmd) {
	case VIRTIO_RPROTO_IOC_XFER:
		if (copy_from_user(&x, uarg, sizeof(x)))
			return -EFAULT;
		ret = vrp_xfer(d, &x);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &x, sizeof(x)))
			return -EFAULT;
		return 0;
	case VIRTIO_RPROTO_IOC_MSG_MAX: {
		__u32 max = VIRTIO_RPROTO_MSG_MAX;

		return copy_to_user(uarg, &max, sizeof(max)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vrp_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= vrp_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= no_llseek,
};

static int vrp_probe(struct virtio_device *vdev)
{
	struct vrp_dev *d;
	int err;

	if (vrp_singleton)
		return -EBUSY;

	d = devm_kzalloc(&vdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->vdev = vdev;
	mutex_init(&d->lock);
	init_completion(&d->done);

	/*
	 * The buffers are handed to the device, so they must come from
	 * memory the DMA API can map — kmalloc'ed, not vmalloc'ed.
	 */
	d->req_buf = devm_kmalloc(&vdev->dev, VIRTIO_RPROTO_MSG_MAX,
				  GFP_KERNEL);
	d->resp_buf = devm_kmalloc(&vdev->dev, VIRTIO_RPROTO_MSG_MAX,
				   GFP_KERNEL);
	if (!d->req_buf || !d->resp_buf)
		return -ENOMEM;

	vdev->priv = d;
	d->vq = virtio_find_single_vq(vdev, vrp_vq_callback, "rproto");
	if (IS_ERR(d->vq))
		return PTR_ERR(d->vq);

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = VIRTIO_RPROTO_DEVNAME;
	d->misc.fops = &vrp_fops;
	d->misc.parent = &vdev->dev;
	err = misc_register(&d->misc);
	if (err) {
		vdev->config->del_vqs(vdev);
		return err;
	}

	vrp_singleton = d;
	dev_info(&vdev->dev, "renderer protocol over virtio, queue size %u\n",
		 virtqueue_get_vring_size(d->vq));
	return 0;
}

static void vrp_remove(struct virtio_device *vdev)
{
	struct vrp_dev *d = vdev->priv;

	vrp_singleton = NULL;
	misc_deregister(&d->misc);
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id vrp_id_table[] = {
	{ 37, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, vrp_id_table);

static struct virtio_driver vrp_driver = {
	.driver.name	= DRV_NAME,
	.driver.owner	= THIS_MODULE,
	.id_table	= vrp_id_table,
	.probe		= vrp_probe,
	.remove		= vrp_remove,
};
module_virtio_driver(vrp_driver);

MODULE_DESCRIPTION("vhost-user guest driver for the renderer protocol");
MODULE_LICENSE("GPL");
