// SPDX-License-Identifier: GPL-2.0
/*
 * infer_rc — RC-side driver for pci_epf_infer (min-latency path)
 *
 * Control registers live in BAR1 (DDR via inbound ATU).
 * Doorbell lives in BAR0 (hardware, typically offset 0xe00).
 *
 * v1: INFER_IOC_XFER uses driver staging buf_dma.
 * v2: INFER_IOC_PUSH + MAP_USER/UNMAP_USER for external/NPU buffers.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#include "infer_proto_v2.h"

#define DRV_NAME "infer_rc"
#define INFER_RC_MAX_USER_MAPS 16

struct infer_user_map {
	bool			in_use;
	struct page		**pages;
	int			nr_pages;
	struct sg_table		sgt;
	bool			sgt_ok;
	dma_addr_t		dma_addr;	/* returned pci_addr */
	size_t			size;
	unsigned long		offset;		/* offset within first page */
};

struct infer_rc {
	struct pci_dev		*pdev;
	void __iomem		*ctrl;
	resource_size_t		ctrl_len;
	void __iomem		*db_iomem;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct miscdevice	misc;
	char			misc_name[32];
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	int			ctrl_bar;
	struct mutex		map_lock;
	struct infer_user_map	maps[INFER_RC_MAX_USER_MAPS];
};

static void infer_ring_doorbell(struct infer_rc *rc)
{
	if (rc->db_iomem)
		writel(rc->db_msg, rc->db_iomem);
}

static int infer_poll_status(struct infer_rc *rc, u64 timeout_ns, u64 *lat_ns)
{
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	ktime_t t0, t1;
	u32 st;

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
	if (lat_ns)
		*lat_ns = ktime_to_ns(ktime_sub(t1, t0));

	if (st == INFER_STATUS_OK)
		return 0;
	if (st == INFER_STATUS_FAIL)
		return -EIO;
	return -ETIMEDOUT;
}

static int infer_do_xfer(struct infer_rc *rc, struct infer_xfer *x)
{
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	u64 timeout_ns = x->timeout_us ? x->timeout_us * 1000ull : 100000000ull;
	u64 lat;
	int ret;

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

	ret = infer_poll_status(rc, timeout_ns, &lat);
	x->timeout_us = lat;
	x->result = ret;
	return ret;
}

static int infer_do_push(struct infer_rc *rc, struct infer_push *p)
{
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	u64 timeout_ns = p->timeout_us ? p->timeout_us * 1000ull : 100000000ull;
	u64 lat;
	int ret;

	if (p->slot >= INFER_V2_SLOTS)
		return -EINVAL;
	if (!p->size || !p->pci_addr)
		return -EINVAL;

	writel(INFER_STATUS_IDLE, &regs->status);
	writel(p->slot, &regs->slot);
	writel(p->size, &regs->size);
	writel(lower_32_bits(p->pci_addr),
	       (void __iomem *)&regs->pci_addr);
	writel(upper_32_bits(p->pci_addr),
	       (void __iomem *)&regs->pci_addr + 4);
	wmb();
	writel(INFER_CMD_PUSH, &regs->command);
	wmb();

	ret = infer_poll_status(rc, timeout_ns, &lat);
	p->timeout_us = lat;
	p->result = ret;
	return ret;
}

static void infer_user_map_release(struct infer_rc *rc, struct infer_user_map *m)
{
	if (!m->in_use)
		return;

	if (m->sgt_ok) {
		dma_unmap_sg(&rc->pdev->dev, m->sgt.sgl, m->sgt.nents, DMA_TO_DEVICE);
		sg_free_table(&m->sgt);
		m->sgt_ok = false;
	}
	if (m->pages) {
		unpin_user_pages_dirty_lock(m->pages, m->nr_pages, true);
		kfree(m->pages);
		m->pages = NULL;
	}
	m->nr_pages = 0;
	m->dma_addr = 0;
	m->size = 0;
	m->in_use = false;
}

static int infer_map_user(struct infer_rc *rc, struct infer_map_req *req)
{
	struct infer_user_map *m = NULL;
	unsigned long uaddr, offset, end;
	int nr_pages, pinned, i, nents, ret;
	struct page **pages;

	if (!req->user_ptr || !req->size)
		return -EINVAL;
	if (req->size > INFER_MAX_XFER)
		return -E2BIG;

	uaddr = (unsigned long)req->user_ptr;
	offset = uaddr & ~PAGE_MASK;
	end = uaddr + req->size;
	nr_pages = (offset + req->size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (nr_pages <= 0)
		return -EINVAL;

	mutex_lock(&rc->map_lock);
	for (i = 0; i < INFER_RC_MAX_USER_MAPS; i++) {
		if (!rc->maps[i].in_use) {
			m = &rc->maps[i];
			break;
		}
	}
	if (!m) {
		mutex_unlock(&rc->map_lock);
		return -ENOSPC;
	}

	pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		mutex_unlock(&rc->map_lock);
		return -ENOMEM;
	}

	pinned = pin_user_pages(uaddr & PAGE_MASK, nr_pages,
				 FOLL_WRITE | FOLL_LONGTERM, pages);
	if (pinned != nr_pages) {
		if (pinned > 0)
			unpin_user_pages(pages, pinned);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return pinned < 0 ? pinned : -EFAULT;
	}

	/* eDMA prep_slave_single needs one contiguous DMA segment */
	for (i = 1; i < nr_pages; i++) {
		if (page_to_pfn(pages[i]) != page_to_pfn(pages[i - 1]) + 1) {
			unpin_user_pages_dirty_lock(pages, nr_pages, false);
			kfree(pages);
			mutex_unlock(&rc->map_lock);
			return -EINVAL; /* not physically contiguous */
		}
	}

	ret = sg_alloc_table_from_pages(&m->sgt, pages, nr_pages, offset,
					req->size, GFP_KERNEL);
	if (ret) {
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return ret;
	}

	nents = dma_map_sg(&rc->pdev->dev, m->sgt.sgl, m->sgt.nents, DMA_TO_DEVICE);
	if (nents <= 0) {
		sg_free_table(&m->sgt);
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return nents < 0 ? nents : -EIO;
	}
	if (nents != 1) {
		dma_unmap_sg(&rc->pdev->dev, m->sgt.sgl, m->sgt.nents, DMA_TO_DEVICE);
		sg_free_table(&m->sgt);
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return -EINVAL; /* IOMMU split into multiple segments */
	}

	m->in_use = true;
	m->pages = pages;
	m->nr_pages = nr_pages;
	m->sgt_ok = true;
	m->offset = offset;
	m->size = req->size;
	m->dma_addr = sg_dma_address(m->sgt.sgl);
	req->pci_addr = m->dma_addr;
	mutex_unlock(&rc->map_lock);

	dev_dbg(&rc->pdev->dev, "MAP_USER va=%#llx size=%llu -> pci=%pad\n",
		req->user_ptr, req->size, &m->dma_addr);
	return 0;
}

static int infer_unmap_user(struct infer_rc *rc, u64 pci_addr)
{
	int i, ret = -ENOENT;

	if (!pci_addr)
		return -EINVAL;

	mutex_lock(&rc->map_lock);
	for (i = 0; i < INFER_RC_MAX_USER_MAPS; i++) {
		if (rc->maps[i].in_use && rc->maps[i].dma_addr == (dma_addr_t)pci_addr) {
			infer_user_map_release(rc, &rc->maps[i]);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&rc->map_lock);
	return ret;
}

static void infer_release_all_maps(struct infer_rc *rc)
{
	int i;

	mutex_lock(&rc->map_lock);
	for (i = 0; i < INFER_RC_MAX_USER_MAPS; i++)
		infer_user_map_release(rc, &rc->maps[i]);
	mutex_unlock(&rc->map_lock);
}

static long infer_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct infer_rc *rc = filp->private_data;
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	struct infer_db_info db;
	struct infer_buf_req br;
	struct infer_xfer x;
	struct infer_push push;
	struct infer_credit credit;
	struct infer_map_req mapreq;
	__u64 pci_addr;
	void __user *uarg = (void __user *)arg;
	int ret;

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

	case INFER_IOC_PUSH:
		if (copy_from_user(&push, uarg, sizeof(push)))
			return -EFAULT;
		infer_do_push(rc, &push);
		if (copy_to_user(uarg, &push, sizeof(push)))
			return -EFAULT;
		return push.result;

	case INFER_IOC_GET_CREDIT:
		credit.posted_mask = readl(&regs->posted_mask);
		credit.done_mask = readl(&regs->done_mask);
		credit.seq = readl(&regs->seq);
		credit.ep_flags = readl(&regs->ep_flags);
		if (copy_to_user(uarg, &credit, sizeof(credit)))
			return -EFAULT;
		return 0;

	case INFER_IOC_MAP_USER:
		if (copy_from_user(&mapreq, uarg, sizeof(mapreq)))
			return -EFAULT;
		ret = infer_map_user(rc, &mapreq);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &mapreq, sizeof(mapreq))) {
			infer_unmap_user(rc, mapreq.pci_addr);
			return -EFAULT;
		}
		return 0;

	case INFER_IOC_UNMAP_USER:
		if (copy_from_user(&pci_addr, uarg, sizeof(pci_addr)))
			return -EFAULT;
		return infer_unmap_user(rc, pci_addr);

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
	struct infer_regs_v2 __iomem *regs;
	u32 magic, ep_flags;
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
	mutex_init(&rc->map_lock);

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
	ep_flags = readl(&regs->ep_flags);

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
		 "infer_rc ready ctrl=BAR%d db=%u:0x%x ep_flags=0x%x MAP_USER /dev/%s\n",
		 rc->ctrl_bar, rc->db_bar, rc->db_offset, ep_flags, rc->misc_name);
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
	infer_release_all_maps(rc);
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
MODULE_DESCRIPTION("RC driver for pci_epf_infer (XFER/PUSH/MAP_USER)");
MODULE_LICENSE("GPL");
