// SPDX-License-Identifier: GPL-2.0
/*
 * infer_rc — RC-side driver for pci_epf_infer (min-latency path)
 *
 * Control registers live in BAR1 (DDR via inbound ATU).
 * Doorbell lives in BAR0 (hardware, typically offset 0xe00).
 * Critical path: write command regs -> ring doorbell -> poll status.
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
	void __iomem		*ctrl;		/* BAR1: protocol regs */
	resource_size_t		ctrl_len;
	void __iomem		*db_iomem;	/* BAR0+offset: doorbell */
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct miscdevice	misc;
	char			misc_name[32];
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	int			ctrl_bar;
};

static void infer_ring_doorbell(struct infer_rc *rc)
{
	if (rc->db_iomem)
		writel(rc->db_msg, rc->db_iomem);
}

static int infer_do_xfer(struct infer_rc *rc, struct infer_xfer *x)
{
	struct infer_regs __iomem *regs = rc->ctrl;
	ktime_t t0, t1;
	u64 timeout_ns = x->timeout_us ? x->timeout_us * 1000ull : 100000000ull;
	u32 st;

	if (x->cmd != INFER_CMD_WRITE && x->cmd != INFER_CMD_READ)
		return -EINVAL;
	if (!x->size || x->size > rc->buf_size)
		return -EINVAL;

	writel(INFER_STATUS_IDLE, &regs->status);
	writel(x->size, &regs->size);
	writel(lower_32_bits(rc->buf_dma),
	       (void __iomem *)&regs->pci_addr);
	writel(upper_32_bits(rc->buf_dma),
	       (void __iomem *)&regs->pci_addr + 4);
	wmb();
	writel(x->cmd, &regs->command);
	wmb();

	t0 = ktime_get();
	infer_ring_doorbell(rc);

	do {
		st = readl(&regs->status);
		if (st == INFER_STATUS_OK || st == INFER_STATUS_FAIL)
			break;
		cpu_relax();
		t1 = ktime_get();
	} while (ktime_to_ns(ktime_sub(t1, t0)) < timeout_ns);

	t1 = ktime_get();
	x->timeout_us = ktime_to_ns(ktime_sub(t1, t0));

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

static int infer_map_doorbell(struct infer_rc *rc)
{
	struct pci_dev *pdev = rc->pdev;
	int bar = rc->db_bar;
	void __iomem *base;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS)
		return -EINVAL;
	if (!pci_resource_len(pdev, bar))
		return -ENODEV;

	base = pci_iomap(pdev, bar, 0);
	if (!base)
		return -ENOMEM;

	if (rc->db_offset >= pci_resource_len(pdev, bar)) {
		pci_iounmap(pdev, base);
		return -EINVAL;
	}
	rc->db_iomem = base + rc->db_offset;
	dev_info(&pdev->dev, "doorbell mapped BAR%d+0x%x\n", bar, rc->db_offset);
	return 0;
}

static int infer_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct infer_rc *rc;
	struct infer_regs __iomem *regs;
	u32 magic;
	int ret, bar;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	rc = devm_kzalloc(&pdev->dev, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	rc->pdev = pdev;
	rc->buf_size = INFER_MAX_XFER;
	rc->ctrl_bar = -1;

	/* Prefer BAR1 (protocol); fall back to scanning all BARs for magic. */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		int try = (bar == 0) ? INFER_CTRL_BARNO :
			  (bar <= INFER_CTRL_BARNO ? bar - 1 : bar);

		if (try < 0 || !pci_resource_len(pdev, try))
			continue;
		if (!(pci_resource_flags(pdev, try) & IORESOURCE_MEM))
			continue;

		rc->ctrl = pci_iomap(pdev, try, 0);
		if (!rc->ctrl) {
			dev_warn(&pdev->dev, "pci_iomap BAR%d failed\n", try);
			continue;
		}
		rc->ctrl_len = pci_resource_len(pdev, try);
		magic = readl(rc->ctrl);
		if (magic == INFER_MAGIC) {
			rc->ctrl_bar = try;
			dev_info(&pdev->dev, "infer regs on BAR%d len=%pa\n",
				 try, &rc->ctrl_len);
			break;
		}
		dev_warn(&pdev->dev, "BAR%d magic=0x%x want=0x%x\n",
			 try, magic, INFER_MAGIC);
		pci_iounmap(pdev, rc->ctrl);
		rc->ctrl = NULL;
	}
	if (!rc->ctrl) {
		dev_err(&pdev->dev,
			"no BAR with magic 0x%x (EP reprogram after start? ctrl on BAR1)\n",
			INFER_MAGIC);
		return -EINVAL;
	}

	regs = rc->ctrl;
	rc->db_bar = readl(&regs->db_bar);
	rc->db_offset = readl(&regs->db_offset);
	rc->db_msg = readl(&regs->db_msg);

	ret = infer_map_doorbell(rc);
	if (ret) {
		dev_err(&pdev->dev, "map doorbell BAR%d+0x%x failed: %d\n",
			rc->db_bar, rc->db_offset, ret);
		goto err_unmap_ctrl;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_unmap_db;

	rc->buf = dmam_alloc_coherent(&pdev->dev, rc->buf_size, &rc->buf_dma,
				      GFP_KERNEL);
	if (!rc->buf) {
		ret = -ENOMEM;
		goto err_unmap_db;
	}

	snprintf(rc->misc_name, sizeof(rc->misc_name), "infer_rc%d",
		 pci_domain_nr(pdev->bus));
	rc->misc.minor = MISC_DYNAMIC_MINOR;
	rc->misc.name = rc->misc_name;
	rc->misc.fops = &infer_fops;
	rc->misc.parent = &pdev->dev;

	ret = misc_register(&rc->misc);
	if (ret)
		goto err_unmap_db;

	pci_set_drvdata(pdev, rc);
	dev_info(&pdev->dev,
		 "infer_rc ready ctrl=BAR%d db=%u:0x%x msg=0x%x buf=%pad /dev/%s\n",
		 rc->ctrl_bar, rc->db_bar, rc->db_offset, rc->db_msg, &rc->buf_dma,
		 rc->misc_name);
	return 0;

err_unmap_db:
	if (rc->db_iomem)
		pci_iounmap(pdev, rc->db_iomem - rc->db_offset);
err_unmap_ctrl:
	pci_iounmap(pdev, rc->ctrl);
	return ret;
}

static void infer_remove(struct pci_dev *pdev)
{
	struct infer_rc *rc = pci_get_drvdata(pdev);

	misc_deregister(&rc->misc);
	if (rc->db_iomem)
		pci_iounmap(pdev, rc->db_iomem - rc->db_offset);
	if (rc->ctrl)
		pci_iounmap(pdev, rc->ctrl);
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
