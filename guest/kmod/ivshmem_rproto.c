// SPDX-License-Identifier: GPL-2.0-only
/*
 * ivshmem_rproto - guest driver for QEMU's ivshmem-doorbell device.
 *
 * Phase 3 of this project: replace the Phase 2 polling loop with
 * interrupt-driven notification. The device (1af4:1110) exposes
 *
 *   BAR0  registers (Interrupt Mask, Interrupt Status, IVPosition,
 *         Doorbell), 256 bytes
 *   BAR1  MSI-X table
 *   BAR2  the shared memory region
 *
 * A peer rings us by writing (our_id << 16) | vector to its own Doorbell
 * register, which raises MSI-X vector `vector` here. We count the
 * interrupt and wake everyone blocked on the character device; userspace
 * then drains the shared-memory ring.
 *
 * Why a character device rather than a UIO driver: uio_pci_generic cannot
 * be used at all because it only handles INTx while the doorbell is
 * MSI-X. A custom UIO driver would handle MSI-X, but userspace would
 * still have to reach the Doorbell register through a mapping of BAR0 —
 * and BAR0 is 256 bytes, so it is not guaranteed to start on a page
 * boundary, which is exactly what UIO's mmap requires. Keeping the
 * register writes in the kernel behind an ioctl sidesteps that, and gives
 * us poll() support for free.
 */
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "ivshmem_rproto.h"

#define DRV_NAME "ivshmem_rproto"

#define IVSHM_PCI_VENDOR 0x1af4
#define IVSHM_PCI_DEVICE 0x1110

/*
 * BAR0 register offsets. Interrupt Mask (0x00) and Interrupt Status
 * (0x04) only carry meaning for INTx delivery, which this driver never
 * uses, so they are listed for completeness and left alone.
 */
#define IVSHM_REG_IVPOSITION 0x08
#define IVSHM_REG_DOORBELL   0x0c

#define IVSHM_BAR_REGS 0
#define IVSHM_BAR_SHM  2

struct ivshm_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	phys_addr_t shm_phys;
	resource_size_t shm_size;
	int irq;
	atomic_t events;
	wait_queue_head_t wq;
	struct miscdevice misc;
};

/* Per-open state: the event count this opener has already observed. */
struct ivshm_file {
	struct ivshm_dev *dev;
	u32 last_seen;
};

static irqreturn_t ivshm_irq_handler(int irq, void *arg)
{
	struct ivshm_dev *dev = arg;

	/*
	 * MSI-X needs no acknowledge in the device: the Interrupt Status
	 * register is only meaningful for INTx. Count the event and wake
	 * the readers.
	 */
	atomic_inc(&dev->events);
	wake_up_interruptible(&dev->wq);
	return IRQ_HANDLED;
}

static int ivshm_open(struct inode *inode, struct file *file)
{
	struct ivshm_dev *dev = container_of(file->private_data,
					     struct ivshm_dev, misc);
	struct ivshm_file *pf;

	pf = kzalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return -ENOMEM;
	pf->dev = dev;
	/*
	 * Start from the current count so a reader only ever sees
	 * interrupts that arrive after it opened the device.
	 */
	pf->last_seen = (u32)atomic_read(&dev->events);
	file->private_data = pf;
	return 0;
}

static int ivshm_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t ivshm_read(struct file *file, char __user *buf, size_t len,
			  loff_t *ppos)
{
	struct ivshm_file *pf = file->private_data;
	struct ivshm_dev *dev = pf->dev;
	u32 count;

	if (len < sizeof(count))
		return -EINVAL;

	count = (u32)atomic_read(&dev->events);
	if (count == pf->last_seen) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(dev->wq,
					     (count = (u32)atomic_read(&dev->events))
					     != pf->last_seen))
			return -ERESTARTSYS;
	}
	pf->last_seen = count;
	if (copy_to_user(buf, &count, sizeof(count)))
		return -EFAULT;
	return sizeof(count);
}

static __poll_t ivshm_poll(struct file *file, poll_table *wait)
{
	struct ivshm_file *pf = file->private_data;
	struct ivshm_dev *dev = pf->dev;

	poll_wait(file, &dev->wq, wait);
	if ((u32)atomic_read(&dev->events) != pf->last_seen)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static long ivshm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ivshm_file *pf = file->private_data;
	struct ivshm_dev *dev = pf->dev;
	void __user *uarg = (void __user *)arg;
	u32 val32;
	u64 val64;

	switch (cmd) {
	case IVSHM_IOC_RING:
		if (get_user(val32, (u32 __user *)uarg))
			return -EFAULT;
		writel(val32, dev->regs + IVSHM_REG_DOORBELL);
		return 0;
	case IVSHM_IOC_GET_ID:
		val32 = readl(dev->regs + IVSHM_REG_IVPOSITION);
		return put_user(val32, (u32 __user *)uarg);
	case IVSHM_IOC_SHM_SIZE:
		val64 = dev->shm_size;
		return put_user(val64, (u64 __user *)uarg);
	default:
		return -ENOTTY;
	}
}

static int ivshm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ivshm_file *pf = file->private_data;
	struct ivshm_dev *dev = pf->dev;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff != 0)
		return -EINVAL;
	if (size > dev->shm_size)
		return -EINVAL;
	/*
	 * BAR2 is host RAM handed to us through the device, so we leave
	 * the caching attributes to the architecture rather than forcing
	 * them; the ring's barriers do not depend on the choice.
	 */
	return remap_pfn_range(vma, vma->vm_start,
			       dev->shm_phys >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

static const struct file_operations ivshm_fops = {
	.owner		= THIS_MODULE,
	.open		= ivshm_open,
	.release	= ivshm_release,
	.read		= ivshm_read,
	.poll		= ivshm_poll,
	.unlocked_ioctl	= ivshm_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= ivshm_mmap,
};

static int ivshm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ivshm_dev *dev;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->pdev = pdev;
	init_waitqueue_head(&dev->wq);
	atomic_set(&dev->events, 0);

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "cannot enable device: %d\n", ret);
		return ret;
	}
	/*
	 * MSI-X is delivered as a memory write issued by the device, so it
	 * only reaches us once the device is a bus master. Without this the
	 * doorbell silently never fires and both sides fall back to their
	 * polling timeouts.
	 */
	pci_set_master(pdev);

	if (pci_resource_len(pdev, IVSHM_BAR_SHM) == 0) {
		dev_err(&pdev->dev,
			"BAR%d is empty; this looks like ivshmem without shared memory\n",
			IVSHM_BAR_SHM);
		return -ENODEV;
	}

	/* Registers are only touched from the kernel, so map just BAR0. */
	ret = pcim_iomap_regions(pdev, BIT(IVSHM_BAR_REGS), DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "cannot map registers: %d\n", ret);
		return ret;
	}
	dev->regs = pcim_iomap_table(pdev)[IVSHM_BAR_REGS];
	dev->shm_phys = pci_resource_start(pdev, IVSHM_BAR_SHM);
	dev->shm_size = pci_resource_len(pdev, IVSHM_BAR_SHM);

	/*
	 * The doorbell is MSI-X only. Asking for a single vector is enough
	 * for this protocol: one ring per direction, one wakeup reason.
	 */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"no MSI-X vector (is the device ivshmem-doorbell with vectors>0?): %d\n",
			ret);
		return ret;
	}
	dev->irq = pci_irq_vector(pdev, 0);
	/*
	 * Requested and freed by hand rather than through devm: the vectors
	 * must outlive the handler, and devres would release them the other
	 * way round, freeing an MSI-X vector that still has an action.
	 */
	ret = request_irq(dev->irq, ivshm_irq_handler, 0, DRV_NAME, dev);
	if (ret) {
		dev_err(&pdev->dev, "cannot request irq %d: %d\n",
			dev->irq, ret);
		goto err_vectors;
	}

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = IVSHM_RPROTO_DEVNAME;
	dev->misc.fops = &ivshm_fops;
	dev->misc.parent = &pdev->dev;
	ret = misc_register(&dev->misc);
	if (ret) {
		dev_err(&pdev->dev, "cannot register %s: %d\n",
			IVSHM_RPROTO_DEVPATH, ret);
		goto err_irq;
	}

	pci_set_drvdata(pdev, dev);
	dev_info(&pdev->dev,
		 "%s ready: peer id %u, shm %llu bytes at %pa, irq %d\n",
		 IVSHM_RPROTO_DEVPATH,
		 readl(dev->regs + IVSHM_REG_IVPOSITION),
		 (unsigned long long)dev->shm_size, &dev->shm_phys, dev->irq);
	return 0;

err_irq:
	free_irq(dev->irq, dev);
err_vectors:
	pci_free_irq_vectors(pdev);
	return ret;
}

static void ivshm_remove(struct pci_dev *pdev)
{
	struct ivshm_dev *dev = pci_get_drvdata(pdev);

	misc_deregister(&dev->misc);
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id ivshm_id_table[] = {
	{ PCI_DEVICE(IVSHM_PCI_VENDOR, IVSHM_PCI_DEVICE) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ivshm_id_table);

static struct pci_driver ivshm_driver = {
	.name		= DRV_NAME,
	.id_table	= ivshm_id_table,
	.probe		= ivshm_probe,
	.remove		= ivshm_remove,
};
module_pci_driver(ivshm_driver);

MODULE_DESCRIPTION("ivshmem-doorbell guest driver for the renderer protocol");
MODULE_LICENSE("GPL");
