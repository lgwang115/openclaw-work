// SPDX-License-Identifier: GPL-2.0
/*
 * infer_rc — RC-side driver for pci_epf_infer (min-latency path)
 *
 * Control registers live in BAR1 (DDR via inbound ATU).
 * Doorbell lives in BAR0 (hardware, typically offset 0xe00).
 *
 * v1: INFER_IOC_XFER uses driver staging buf_dma.
 * v2: INFER_IOC_PUSH + MAP_USER/MAP_DMABUF for external/NPU buffers.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>

#include "infer_proto_v2.h"

#define DRV_NAME "infer_rc"
#define INFER_RC_MAX_MAPS 16

enum infer_map_kind {
	INFER_MAP_NONE = 0,
	INFER_MAP_USER,
	INFER_MAP_DMABUF,
};

struct infer_rc_map {
	enum infer_map_kind	kind;
	dma_addr_t		dma_addr;
	size_t			size;
	/* USER */
	struct page		**pages;
	int			nr_pages;
	struct sg_table		user_sgt;
	bool			user_sgt_ok;
	/* DMABUF */
	struct dma_buf		*dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table		*db_sgt;
};

struct infer_rc {
	struct pci_dev		*pdev;
	void __iomem		*ctrl;
	resource_size_t		ctrl_len;
	void __iomem		*db_iomem;
	void __iomem		*db_bar_base; /* full BAR0 map (doorbell+HDMA live here) */
	void __iomem		*hdma_iomem;  /* db_bar_base + BST_TRGT0_HDMA_BASE */
	size_t			hdma_len;     /* bytes mapped from HDMA base to BAR end */
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
	struct infer_rc_map	maps[INFER_RC_MAX_MAPS];
};

static void infer_ring_doorbell(struct infer_rc *rc)
{
	if (rc->db_iomem)
		writel(rc->db_msg, rc->db_iomem);
}

/*
 * Poll for completion. Fast path (use_hwdone): the EP eDMA writes
 * INFER_HW_DONE_MAGIC into regs->hw_done right after the data lands, so we see
 * it without waiting for the EP completion IRQ/callback. status (callback
 * written) is kept as a fallback / for the non-hwdone path.
 */
static int infer_poll_status(struct infer_rc *rc, u64 timeout_ns, u64 *lat_ns,
			     bool use_hwdone)
{
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	ktime_t t0, t1;
	u32 st = INFER_STATUS_IDLE;
	bool done = false, fail = false;

	t0 = ktime_get();
	infer_ring_doorbell(rc);

	do {
		if (use_hwdone &&
		    readl(&regs->hw_done) == INFER_HW_DONE_MAGIC) {
			done = true;
			break;
		}
		st = readl(&regs->status);
		if (st == INFER_STATUS_OK) {
			done = true;
			break;
		}
		if (st == INFER_STATUS_FAIL) {
			fail = true;
			break;
		}
		cpu_relax();
		t1 = ktime_get();
	} while (ktime_to_ns(ktime_sub(t1, t0)) < timeout_ns);

	t1 = ktime_get();
	if (lat_ns)
		*lat_ns = ktime_to_ns(ktime_sub(t1, t0));

	if (done)
		return 0;
	if (fail)
		return -EIO;
	return -ETIMEDOUT;
}

static int infer_do_xfer(struct infer_rc *rc, struct infer_xfer *x)
{
	struct infer_regs_v2 __iomem *regs = rc->ctrl;
	u64 timeout_ns = x->timeout_us ? x->timeout_us * 1000ull : 100000000ull;
	u64 lat;
	int ret;
	bool use_hwdone = false;

	if (x->cmd != INFER_CMD_WRITE && x->cmd != INFER_CMD_READ)
		return -EINVAL;
	if (!x->size || x->size > rc->buf_size)
		return -EINVAL;

	writel(INFER_STATUS_IDLE, &regs->status);
	writel(0, &regs->hw_done);
	writel(x->size, &regs->size);
	writel(lower_32_bits(rc->buf_dma),
	       (void __iomem *)&regs->pci_addr);
	writel(upper_32_bits(rc->buf_dma),
	       (void __iomem *)&regs->pci_addr + 4);

	/*
	 * WRITE = EP reads our buffer (DEV_TO_MEM). Place the eDMA completion
	 * tag right after the payload so the EP's 2nd LL element copies it into
	 * regs->hw_done — RC then sees completion without the EP IRQ/callback.
	 * Needs room for the 4-byte trailer in the staging buffer.
	 */
	if (x->cmd == INFER_CMD_WRITE && rc->buf &&
	    (size_t)x->size + sizeof(u32) <= rc->buf_size) {
		*(u32 *)((u8 *)rc->buf + x->size) = INFER_HW_DONE_MAGIC;
		writel(INFER_XF_HWDONE, &regs->xfer_flags);
		use_hwdone = true;
	} else {
		writel(0, &regs->xfer_flags);
	}
	wmb();
	writel(x->cmd, &regs->command);
	wmb();

	ret = infer_poll_status(rc, timeout_ns, &lat, use_hwdone);
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
	writel(0, &regs->hw_done);
	writel(0, &regs->xfer_flags);   /* PUSH keeps the callback-status path for now */
	writel(p->slot, &regs->slot);
	writel(p->size, &regs->size);
	writel(lower_32_bits(p->pci_addr),
	       (void __iomem *)&regs->pci_addr);
	writel(upper_32_bits(p->pci_addr),
	       (void __iomem *)&regs->pci_addr + 4);
	wmb();
	writel(INFER_CMD_PUSH, &regs->command);
	wmb();

	ret = infer_poll_status(rc, timeout_ns, &lat, false);
	p->timeout_us = lat;
	p->result = ret;
	return ret;
}

static void infer_rc_map_release(struct infer_rc *rc, struct infer_rc_map *m)
{
	if (m->kind == INFER_MAP_NONE)
		return;

	if (m->kind == INFER_MAP_USER) {
		if (m->user_sgt_ok) {
			dma_unmap_sg(&rc->pdev->dev, m->user_sgt.sgl,
				     m->user_sgt.nents, DMA_TO_DEVICE);
			sg_free_table(&m->user_sgt);
			m->user_sgt_ok = false;
		}
		if (m->pages) {
			unpin_user_pages_dirty_lock(m->pages, m->nr_pages, true);
			kfree(m->pages);
			m->pages = NULL;
		}
		m->nr_pages = 0;
	} else if (m->kind == INFER_MAP_DMABUF) {
		if (m->db_sgt && m->attach) {
			dma_buf_unmap_attachment(m->attach, m->db_sgt,
						 DMA_TO_DEVICE);
			m->db_sgt = NULL;
		}
		if (m->attach && m->dmabuf) {
			dma_buf_detach(m->dmabuf, m->attach);
			m->attach = NULL;
		}
		if (m->dmabuf) {
			dma_buf_put(m->dmabuf);
			m->dmabuf = NULL;
		}
	}

	m->dma_addr = 0;
	m->size = 0;
	m->kind = INFER_MAP_NONE;
}

static struct infer_rc_map *infer_rc_map_alloc_slot(struct infer_rc *rc)
{
	int i;

	for (i = 0; i < INFER_RC_MAX_MAPS; i++) {
		if (rc->maps[i].kind == INFER_MAP_NONE)
			return &rc->maps[i];
	}
	return NULL;
}

static int infer_map_user(struct infer_rc *rc, struct infer_map_req *req)
{
	struct infer_rc_map *m;
	unsigned long uaddr, offset;
	int nr_pages, pinned, i, nents, ret;
	struct page **pages;

	if (!req->user_ptr || !req->size)
		return -EINVAL;
	if (req->size > INFER_MAX_XFER)
		return -E2BIG;

	uaddr = (unsigned long)req->user_ptr;
	offset = uaddr & ~PAGE_MASK;
	nr_pages = (offset + req->size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (nr_pages <= 0)
		return -EINVAL;

	mutex_lock(&rc->map_lock);
	m = infer_rc_map_alloc_slot(rc);
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
		/*
		 * Common failure: VA from dma_mmap_coherent / remap_pfn_range
		 * is VM_PFNMAP and cannot be pinned (-EFAULT).
		 */
		dev_dbg(&rc->pdev->dev,
			"MAP_USER pin_user_pages failed pinned=%d want=%d\n",
			pinned, nr_pages);
		return pinned < 0 ? pinned : -EFAULT;
	}

	for (i = 1; i < nr_pages; i++) {
		if (page_to_pfn(pages[i]) != page_to_pfn(pages[i - 1]) + 1) {
			unpin_user_pages_dirty_lock(pages, nr_pages, false);
			kfree(pages);
			mutex_unlock(&rc->map_lock);
			return -EINVAL;
		}
	}

	ret = sg_alloc_table_from_pages(&m->user_sgt, pages, nr_pages, offset,
					req->size, GFP_KERNEL);
	if (ret) {
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return ret;
	}

	nents = dma_map_sg(&rc->pdev->dev, m->user_sgt.sgl, m->user_sgt.nents,
			   DMA_TO_DEVICE);
	if (nents <= 0) {
		sg_free_table(&m->user_sgt);
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return nents < 0 ? nents : -EIO;
	}
	if (nents != 1) {
		dma_unmap_sg(&rc->pdev->dev, m->user_sgt.sgl, m->user_sgt.nents,
			     DMA_TO_DEVICE);
		sg_free_table(&m->user_sgt);
		unpin_user_pages_dirty_lock(pages, nr_pages, false);
		kfree(pages);
		mutex_unlock(&rc->map_lock);
		return -EINVAL;
	}

	m->kind = INFER_MAP_USER;
	m->pages = pages;
	m->nr_pages = nr_pages;
	m->user_sgt_ok = true;
	m->size = req->size;
	m->dma_addr = sg_dma_address(m->user_sgt.sgl);
	req->pci_addr = m->dma_addr;
	mutex_unlock(&rc->map_lock);

	dev_dbg(&rc->pdev->dev, "MAP_USER va=%#llx size=%llu -> pci=%pad\n",
		req->user_ptr, req->size, &m->dma_addr);
	return 0;
}

static int infer_map_dmabuf(struct infer_rc *rc, struct infer_map_dmabuf *req)
{
	struct infer_rc_map *m;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t base;
	size_t dma_len, want, avail;

	if (req->dmabuf_fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(req->dmabuf_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	if (req->dmabuf_offset >= dmabuf->size) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	want = req->size ? req->size : (dmabuf->size - req->dmabuf_offset);
	if (!want || want > INFER_MAX_XFER) {
		dma_buf_put(dmabuf);
		return want ? -E2BIG : -EINVAL;
	}
	if (req->dmabuf_offset + want > dmabuf->size) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	attach = dma_buf_attach(dmabuf, &rc->pdev->dev);
	if (IS_ERR(attach)) {
		dma_buf_put(dmabuf);
		return PTR_ERR(attach);
	}

	sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE);
	if (IS_ERR(sgt)) {
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return PTR_ERR(sgt);
	}

	if (sgt->nents != 1) {
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	base = sg_dma_address(sgt->sgl);
	dma_len = sg_dma_len(sgt->sgl);
	if (req->dmabuf_offset >= dma_len) {
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}
	avail = dma_len - req->dmabuf_offset;
	if (want > avail) {
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	mutex_lock(&rc->map_lock);
	m = infer_rc_map_alloc_slot(rc);
	if (!m) {
		mutex_unlock(&rc->map_lock);
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -ENOSPC;
	}

	m->kind = INFER_MAP_DMABUF;
	m->dmabuf = dmabuf;
	m->attach = attach;
	m->db_sgt = sgt;
	m->size = want;
	m->dma_addr = base + req->dmabuf_offset;
	req->pci_addr = m->dma_addr;
	req->size = want;
	mutex_unlock(&rc->map_lock);

	dev_dbg(&rc->pdev->dev,
		"MAP_DMABUF fd=%d off=%u size=%zu -> pci=%pad\n",
		req->dmabuf_fd, req->dmabuf_offset, want, &m->dma_addr);
	return 0;
}

static int infer_unmap_by_pci(struct infer_rc *rc, u64 pci_addr)
{
	int i, ret = -ENOENT;

	if (!pci_addr)
		return -EINVAL;

	mutex_lock(&rc->map_lock);
	for (i = 0; i < INFER_RC_MAX_MAPS; i++) {
		if (rc->maps[i].kind != INFER_MAP_NONE &&
		    rc->maps[i].dma_addr == (dma_addr_t)pci_addr) {
			infer_rc_map_release(rc, &rc->maps[i]);
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
	for (i = 0; i < INFER_RC_MAX_MAPS; i++)
		infer_rc_map_release(rc, &rc->maps[i]);
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
	struct infer_map_dmabuf mapdb;
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
			infer_unmap_by_pci(rc, mapreq.pci_addr);
			return -EFAULT;
		}
		return 0;

	case INFER_IOC_UNMAP_USER:
	case INFER_IOC_UNMAP_DMABUF:
		if (copy_from_user(&pci_addr, uarg, sizeof(pci_addr)))
			return -EFAULT;
		return infer_unmap_by_pci(rc, pci_addr);

	case INFER_IOC_MAP_DMABUF:
		if (copy_from_user(&mapdb, uarg, sizeof(mapdb)))
			return -EFAULT;
		ret = infer_map_dmabuf(rc, &mapdb);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &mapdb, sizeof(mapdb))) {
			infer_unmap_by_pci(rc, mapdb.pci_addr);
			return -EFAULT;
		}
		return 0;

	case INFER_IOC_HDMA_DBG: {
		struct infer_hdma_dbg dbg;

		if (copy_from_user(&dbg, uarg, sizeof(dbg)))
			return -EFAULT;
		if (!rc->hdma_iomem)
			return -ENODEV;
		if ((dbg.offset & 0x3) || dbg.offset + 4 > rc->hdma_len)
			return -EINVAL;
		if (dbg.is_write)
			writel(dbg.value, rc->hdma_iomem + dbg.offset);
		dbg.out = readl(rc->hdma_iomem + dbg.offset);
		if (copy_to_user(uarg, &dbg, sizeof(dbg)))
			return -EFAULT;
		return 0;
	}

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
	rc->db_bar_base = base;
	rc->db_iomem = base + rc->db_offset;
	dev_info(&pdev->dev, "doorbell mapped BAR%d+0x%x\n", bar, rc->db_offset);

	/* HDMA regs live in the same BAR0 target0 window at 0x4000 */
	if (pci_resource_len(pdev, bar) > INFER_HDMA_BASE) {
		rc->hdma_iomem = base + INFER_HDMA_BASE;
		rc->hdma_len = pci_resource_len(pdev, bar) - INFER_HDMA_BASE;
		dev_info(&pdev->dev, "HDMA regs mapped BAR%d+0x%x len=0x%zx\n",
			 bar, INFER_HDMA_BASE, rc->hdma_len);
	}
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
		 "infer_rc ready ctrl=BAR%d db=%u:0x%x ep_flags=0x%x MAP_USER/DMABUF /dev/%s\n",
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
MODULE_DESCRIPTION("RC driver for pci_epf_infer (XFER/PUSH/MAP_USER/MAP_DMABUF)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
