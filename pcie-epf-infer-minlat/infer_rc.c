// SPDX-License-Identifier: GPL-2.0
/*
 * infer_rc — RC-side driver for pci_epf_infer (min-latency path)
 *
 * Critical path: write command regs -> ring doorbell -> poll status.
 * No MSI wait on the data path.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#include "infer_proto.h"

#define DRV_NAME "infer_rc"

struct infer_rc {
	struct pci_dev		*pdev;
	void __iomem		*bar0;
	resource_size_t		bar0_len;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct miscdevice	misc;
	char			misc_name[32];
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	void __iomem		*db_iomem;	/* doorbell MMIO if same BAR0 */
};

static inline u32 ir_readl(struct infer_rc *rc, unsigned int off)
{
	return readl(rc->bar0 + off);
}

static inline void ir_writel(struct infer_rc *rc, unsigned int off, u32 v)
{
	writel(v, rc->bar0 + off);
}

static void infer_ring_doorbell(struct infer_rc *rc)
{
	/*
	 * BST doorbell: write msg to BAR+offset.
	 * On A2000 pci_epf_test this is typically BAR0 + 0xe00.
	 */
	if (rc->db_iomem)
		writel(rc->db_msg, rc->db_iomem);
	else if (rc->db_bar == 0)
		writel(rc->db_msg, rc->bar0 + rc->db_offset);
}

static int infer_do_xfer(struct infer_rc *rc, struct infer_xfer *x)
{
	struct infer_regs __iomem *regs = rc->bar0;
	ktime_t t0, t1;
	u64 timeout_ns = x->timeout_us ? x->timeout_us * 1000ull : 100000000ull;
	u32 st;

	if (x->cmd != INFER_CMD_WRITE && x->cmd != INFER_CMD_READ)
		return -EINVAL;
	if (!x->size || x->size > rc->buf_size)
		return -EINVAL;

	/* Clear completion, program command, then ring doorbell last. */
	writel(INFER_STATUS_IDLE, &regs->status);
	writel(x->size, &regs->size);
	writeq(rc->buf_dma, &regs->pci_addr);
	wmb();
	writel(x->cmd, &regs->command);
	wmb();

	t0 = ktime_get();
	infer_ring_doorbell(rc);

	/* Poll status — minimum latency completion path (no IRQ). */
	do {
		st = readl(&regs->status);
		if (st == INFER_STATUS_OK || st == INFER_STATUS_FAIL)
			break;
		cpu_relax();
		t1 = ktime_get();
	} while (ktime_to_ns(ktime_sub(t1, t0)) < timeout_ns);

	t1 = ktime_get();
	x->timeout_us = ktime_to_ns(ktime_sub(t1, t0)); /* reuse field: latency ns */

	if (st == INFER_STATUS_OK) {
		x->result = 0;
		return 0;
	}
	if (st == INFER_STATUS_FAIL) {
		x->result = -EIO;
		return -EIO;
	}
	x->result = -ETIMEDOUT;
	return -ETIMEDOUT;
}

static long infer_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct infer_rc *rc = filp->private_data;
	struct infer_db_info db;
	struct infer_buf_req br;
	struct infer_xfer x;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case INFER_IOC_GET_DB:
		db.bar = rc->db_bar;
		db.offset = rc->db_offset;
		db.msg = rc->db_msg;
		if (copy_to_user(uarg, &db, sizeof(db)))
			return -EFAULT;
		return 0;

	case INFER_IOC_ALLOC:
		if (copy_from_user(&br, uarg, sizeof(br)))
			return -EFAULT;
		br.size = rc->buf_size;
		br.dma_addr = rc->buf_dma;
		br.user_ptr = 0;
		if (copy_to_user(uarg, &br, sizeof(br)))
			return -EFAULT;
		return 0;

	case INFER_IOC_XFER:
		if (copy_from_user(&x, uarg, sizeof(x)))
			return -EFAULT;
		infer_do_xfer(rc, &x);
		if (copy_to_user(uarg, &x, sizeof(x)))
			return -EFAULT;
		return x.result;

	default:
		return -ENOTTY;
	}
}

static int infer_open(struct inode *inode, struct file *filp)
{
	/* miscdevice sets private_data to &misc before calling open */
	struct infer_rc *rc = container_of(filp->private_data, struct infer_rc, misc);

	filp->private_data = rc;
	return 0;
}

static int infer_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct infer_rc *rc = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > rc->buf_size)
		return -EINVAL;
	return dma_mmap_coherent(&rc->pdev->dev, vma, rc->buf, rc->buf_dma, size);
}

static const struct file_operations infer_fops = {
	.owner		= THIS_MODULE,
	.open		= infer_open,
	.unlocked_ioctl	= infer_ioctl,
	.mmap		= infer_mmap,
};

static int infer_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct infer_rc *rc;
	struct infer_regs __iomem *regs;
	u32 magic;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret)
		return ret;

	pci_set_master(pdev);

	rc = devm_kzalloc(&pdev->dev, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	rc->pdev = pdev;
	rc->bar0 = pcim_iomap_table(pdev)[0];
	rc->bar0_len = pci_resource_len(pdev, 0);
	rc->buf_size = INFER_MAX_XFER;

	regs = rc->bar0;
	magic = readl(&regs->magic);
	if (magic != INFER_MAGIC) {
		dev_err(&pdev->dev, "bad magic 0x%x (want 0x%x)\n", magic, INFER_MAGIC);
		return -ENODEV;
	}

	rc->db_bar = readl(&regs->db_bar);
	rc->db_offset = readl(&regs->db_offset);
	rc->db_msg = readl(&regs->db_msg);
	if (rc->db_bar == 0 && rc->db_offset < rc->bar0_len)
		rc->db_iomem = rc->bar0 + rc->db_offset;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	rc->buf = dmam_alloc_coherent(&pdev->dev, rc->buf_size, &rc->buf_dma,
				      GFP_KERNEL);
	if (!rc->buf)
		return -ENOMEM;

	snprintf(rc->misc_name, sizeof(rc->misc_name), "infer_rc%d",
		 pci_domain_nr(pdev->bus));
	rc->misc.minor = MISC_DYNAMIC_MINOR;
	rc->misc.name = rc->misc_name;
	rc->misc.fops = &infer_fops;
	rc->misc.parent = &pdev->dev;

	ret = misc_register(&rc->misc);
	if (ret)
		return ret;

	pci_set_drvdata(pdev, rc);
	dev_info(&pdev->dev,
		 "infer_rc ready bar0=%p db=%u:0x%x msg=0x%x buf=%pad /dev/%s\n",
		 rc->bar0, rc->db_bar, rc->db_offset, rc->db_msg, &rc->buf_dma,
		 rc->misc_name);
	return 0;
}

static void infer_remove(struct pci_dev *pdev)
{
	struct infer_rc *rc = pci_get_drvdata(pdev);

	misc_deregister(&rc->misc);
}

static const struct pci_device_id infer_ids[] = {
	{ PCI_DEVICE(INFER_VENDOR_ID, INFER_DEVICE_ID) },
	{},
};
MODULE_DEVICE_TABLE(pci, infer_ids);

static struct pci_driver infer_driver = {
	.name		= DRV_NAME,
	.id_table	= infer_ids,
	.probe		= infer_probe,
	.remove		= infer_remove,
};

module_pci_driver(infer_driver);
MODULE_DESCRIPTION("RC driver for min-latency pci_epf_infer");
MODULE_LICENSE("GPL");
