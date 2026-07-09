// SPDX-License-Identifier: GPL-2.0
/*
 * pci_epf_infer — minimum-latency PCIe EP function (A2000 / C1200)
 *
 * Critical: BAR/header/doorbell must be programmed AFTER `echo 1 > start`
 * via /dev/pci_epf_infer_ctl (BST start wipes bind-time BAR/ATU).
 *
 * v1: WRITE/READ against driver staging buffer (inferlat).
 * v2: PUSH → eDMA remote pci_addr → per-slot local_dst (ARM each packet).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "infer_proto_v2.h"
#include "../../controller/bst/pcie-bst.h"

#define DRV_NAME "pci_epf_infer"

struct epf_infer_slot {
	dma_addr_t		local_dst;
	size_t			capacity;
	u32			state;
	u32			xfer_size;
	u32			seq;
	bool			use_staging;
};

struct epf_infer {
	struct pci_epf		*epf;
	struct infer_regs_v2	*regs;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct dma_chan		*dma_tx;
	struct dma_chan		*dma_rx;
	struct completion	xfer_done;
	enum dma_status		xfer_status;
	struct dma_chan		*cur_chan;
	dma_cookie_t		cur_cookie;
	struct work_struct	cmd_work;
	int			db_irq;
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	spinlock_t		lock;
	bool			busy;
	bool			dma_ok;
	struct epf_infer_slot	slots[INFER_V2_SLOTS];
	wait_queue_head_t	slot_wq;
	struct miscdevice	ep_misc;
	bool			ep_misc_registered;
};

static struct epf_infer *g_epf_infer;

static void epf_infer_update_credit_locked(struct epf_infer *ctx)
{
	u32 posted = 0, done = 0;
	int i;

	for (i = 0; i < INFER_V2_SLOTS; i++) {
		if (ctx->slots[i].state == INFER_SLOT_POSTED)
			posted |= BIT(i);
		if (ctx->slots[i].state == INFER_SLOT_DONE)
			done |= BIT(i);
	}
	if (ctx->regs) {
		WRITE_ONCE(ctx->regs->posted_mask, posted);
		WRITE_ONCE(ctx->regs->done_mask, done);
	}
}

static void epf_infer_dma_cb(void *param)
{
	struct epf_infer *ctx = param;

	ctx->xfer_status = dma_async_is_tx_complete(ctx->cur_chan,
						    ctx->cur_cookie, NULL, NULL);
	complete(&ctx->xfer_done);
}

/*
 * eDMA slave xfer: remote PCI addr in slave_config; local addr in prep.
 * local_dma may be staging (ctx->buf_dma) or a per-packet NPU/DDR address.
 */
static int epf_infer_dma_xfer_local(struct epf_infer *ctx,
				    enum dma_transfer_direction dir,
				    dma_addr_t remote, dma_addr_t local,
				    size_t size)
{
	struct dma_chan *chan = (dir == DMA_DEV_TO_MEM) ? ctx->dma_rx : ctx->dma_tx;
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config sconf = {};
	dma_cookie_t cookie;
	int ret;

	if (!chan)
		return -ENODEV;

	sconf.direction = dir;
	if (dir == DMA_MEM_TO_DEV)
		sconf.dst_addr = remote;
	else
		sconf.src_addr = remote;

	ret = dmaengine_slave_config(chan, &sconf);
	if (ret)
		return ret;

	reinit_completion(&ctx->xfer_done);
	ctx->xfer_status = DMA_IN_PROGRESS;
	ctx->cur_chan = chan;

	desc = dmaengine_prep_slave_single(chan, local, size, dir,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc)
		return -EIO;

	desc->callback = epf_infer_dma_cb;
	desc->callback_param = ctx;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return -EIO;
	ctx->cur_cookie = cookie;
	dma_async_issue_pending(chan);

	if (!wait_for_completion_timeout(&ctx->xfer_done, msecs_to_jiffies(100))) {
		dmaengine_terminate_sync(chan);
		return -ETIMEDOUT;
	}
	if (ctx->xfer_status != DMA_COMPLETE)
		return -EIO;
	return 0;
}

static int epf_infer_dma_xfer(struct epf_infer *ctx,
			      enum dma_transfer_direction dir,
			      dma_addr_t remote, size_t size)
{
	return epf_infer_dma_xfer_local(ctx, dir, remote, ctx->buf_dma, size);
}

static void epf_infer_cmd_work(struct work_struct *work)
{
	struct epf_infer *ctx = container_of(work, struct epf_infer, cmd_work);
	struct infer_regs_v2 *regs = ctx->regs;
	struct epf_infer_slot *slot;
	u32 cmd, size, slot_idx, seq;
	u64 pci_addr;
	dma_addr_t local;
	int ret = 0;
	unsigned long flags;

	if (!regs)
		goto out;

	cmd = READ_ONCE(regs->command);
	if (!cmd)
		goto out;

	size = READ_ONCE(regs->size);
	pci_addr = READ_ONCE(regs->pci_addr);
	slot_idx = READ_ONCE(regs->slot);

	WRITE_ONCE(regs->command, INFER_CMD_NONE);
	WRITE_ONCE(regs->status, INFER_STATUS_BUSY);

	switch (cmd) {
	case INFER_CMD_WRITE:
	case INFER_CMD_READ:
		if (!size || size > ctx->buf_size || !ctx->buf) {
			ret = -EINVAL;
			break;
		}
		if (cmd == INFER_CMD_WRITE)
			ret = epf_infer_dma_xfer(ctx, DMA_DEV_TO_MEM, pci_addr, size);
		else
			ret = epf_infer_dma_xfer(ctx, DMA_MEM_TO_DEV, pci_addr, size);
		break;

	case INFER_CMD_PUSH:
		if (slot_idx >= INFER_V2_SLOTS || !size) {
			ret = -EINVAL;
			break;
		}
		spin_lock_irqsave(&ctx->lock, flags);
		slot = &ctx->slots[slot_idx];
		if (slot->state != INFER_SLOT_POSTED) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			ret = -EINVAL;
			break;
		}
		if (size > slot->capacity) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			ret = -EMSGSIZE;
			break;
		}
		local = slot->local_dst;
		slot->state = INFER_SLOT_BUSY;
		epf_infer_update_credit_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);

		ret = epf_infer_dma_xfer_local(ctx, DMA_DEV_TO_MEM, pci_addr,
					       local, size);

		spin_lock_irqsave(&ctx->lock, flags);
		slot = &ctx->slots[slot_idx];
		if (ret) {
			slot->state = INFER_SLOT_ERROR;
		} else {
			seq = READ_ONCE(regs->seq) + 1;
			WRITE_ONCE(regs->seq, seq);
			slot->xfer_size = size;
			slot->seq = seq;
			slot->state = INFER_SLOT_DONE;
		}
		epf_infer_update_credit_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		wake_up_interruptible(&ctx->slot_wq);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	wmb();
	WRITE_ONCE(regs->status, ret ? INFER_STATUS_FAIL : INFER_STATUS_OK);
out:
	spin_lock_irqsave(&ctx->lock, flags);
	ctx->busy = false;
	spin_unlock_irqrestore(&ctx->lock, flags);
}

static int epf_infer_doorbell_handler(int irq, void *arg)
{
	struct epf_infer *ctx = arg;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->busy) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return IRQ_HANDLED;
	}
	ctx->busy = true;
	spin_unlock_irqrestore(&ctx->lock, flags);

	queue_work(system_highpri_wq, &ctx->cmd_work);
	return IRQ_HANDLED;
}

struct epf_dma_filter {
	struct device *dev;
	u32 dma_mask;
};

static bool epf_infer_dma_filter(struct dma_chan *chan, void *arg)
{
	struct epf_dma_filter *filter = arg;
	struct dma_slave_caps caps;

	memset(&caps, 0, sizeof(caps));
	dma_get_slave_caps(chan, &caps);

	return chan->device->dev == filter->dev &&
	       (filter->dma_mask & caps.directions);
}

static int epf_infer_setup_dma(struct epf_infer *ctx)
{
	struct device *dma_dev = ctx->epf->epc->dev.parent;
	struct epf_dma_filter filter;
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	filter.dev = dma_dev;
	filter.dma_mask = BIT(DMA_DEV_TO_MEM);
	ctx->dma_rx = dma_request_channel(mask, epf_infer_dma_filter, &filter);

	filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	ctx->dma_tx = dma_request_channel(mask, epf_infer_dma_filter, &filter);

	if (!ctx->dma_tx || !ctx->dma_rx) {
		dev_err(&ctx->epf->dev,
			"failed to get eDMA slave channels (tx=%p rx=%p)\n",
			ctx->dma_tx, ctx->dma_rx);
		return -ENODEV;
	}

	dev_info(&ctx->epf->dev, "eDMA channels: tx=%s rx=%s\n",
		 dma_chan_name(ctx->dma_tx), dma_chan_name(ctx->dma_rx));

	ctx->buf_size = INFER_MAX_XFER;
	ctx->buf = dma_alloc_coherent(dma_dev, ctx->buf_size, &ctx->buf_dma,
				      GFP_KERNEL);
	if (!ctx->buf)
		return -ENOMEM;

	ctx->dma_ok = true;
	dev_info(&ctx->epf->dev, "DMA ready, staging %zu @ %pad\n",
		 ctx->buf_size, &ctx->buf_dma);
	return 0;
}

static void epf_infer_cleanup_dma(struct epf_infer *ctx)
{
	struct device *dma_dev;

	if (!ctx->epf || !ctx->epf->epc)
		return;
	dma_dev = ctx->epf->epc->dev.parent;

	if (ctx->buf) {
		dma_free_coherent(dma_dev, ctx->buf_size, ctx->buf, ctx->buf_dma);
		ctx->buf = NULL;
	}
	if (ctx->dma_tx) {
		dma_release_channel(ctx->dma_tx);
		ctx->dma_tx = NULL;
	}
	if (ctx->dma_rx) {
		dma_release_channel(ctx->dma_rx);
		ctx->dma_rx = NULL;
	}
	ctx->dma_ok = false;
}

static int epf_infer_setup_doorbell(struct epf_infer *ctx)
{
	struct pci_epf *epf = ctx->epf;
	struct pci_epc *epc = epf->epc;
	int irq, ret;

	irq = bst_pcie_ep_db_irq_alloc(epc, epf->func_no, epf->vfunc_no);
	if (irq < 0)
		return irq;

	ret = bst_pcie_ep_db_irq_request(epc, epf->func_no, epf->vfunc_no, irq,
					 epf_infer_doorbell_handler, ctx);
	if (ret)
		return ret;

	ret = bst_pcie_ep_db_info_get(epc, epf->func_no, epf->vfunc_no, irq,
				      &ctx->db_bar, &ctx->db_offset, &ctx->db_msg);
	if (ret)
		return ret;

	ctx->db_irq = irq;
	dev_info(&epf->dev, "doorbell bar=%u off=0x%x msg=0x%x\n",
		 ctx->db_bar, ctx->db_offset, ctx->db_msg);
	return 0;
}

static int epf_infer_set_ctrl_bar(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	enum pci_barno barno = INFER_CTRL_BARNO;
	struct pci_epf_bar *epf_bar = &epf->bar[barno];
	const struct pci_epc_features *features;
	size_t align = PAGE_SIZE;
	int ret;

	if (!ctx->regs) {
		void *base;

		features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
		if (features && features->align)
			align = features->align;

		memset(epf_bar, 0, sizeof(*epf_bar));
		epf_bar->flags = PCI_BASE_ADDRESS_SPACE_MEMORY |
				 PCI_BASE_ADDRESS_MEM_TYPE_32;
		epf_bar->barno = barno;
		epf_bar->size = INFER_CTRL_BAR_SIZE;

		base = pci_epf_alloc_space(epf, INFER_CTRL_BAR_SIZE, barno,
					   align, PRIMARY_INTERFACE);
		if (!base) {
			dev_err(&epf->dev, "pci_epf_alloc_space BAR%d failed\n",
				barno);
			return -ENOMEM;
		}
		ctx->regs = base;
	}

	memset(ctx->regs, 0, sizeof(*ctx->regs));
	ctx->regs->magic = INFER_MAGIC;
	ctx->regs->status = INFER_STATUS_IDLE;
	ctx->regs->ep_flags = INFER_EP_F_ZEROCOPY | INFER_EP_F_MULTI_SLOT;

	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
	if (ret) {
		dev_err(&epf->dev, "pci_epc_set_bar BAR%d failed: %d\n",
			barno, ret);
		return ret;
	}

	dev_info(&epf->dev, "ctrl BAR%d programmed size=%llu phys=%pap\n",
		 barno, (unsigned long long)epf_bar->size, &epf_bar->phys_addr);
	return 0;
}

static int epf_infer_reprogram(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	int ret, i;

	dev_info(&epf->dev, "reprogram: header/BAR/doorbell (post-start)\n");

	ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, epf->header);
	if (ret) {
		dev_err(&epf->dev, "write_header failed: %d\n", ret);
		return ret;
	}

	ret = epf_infer_set_ctrl_bar(epf);
	if (ret)
		return ret;

	if (!ctx->dma_ok) {
		ret = epf_infer_setup_dma(ctx);
		if (ret)
			dev_err(&epf->dev, "DMA setup failed: %d\n", ret);
	}

	if (ctx->db_irq < 0) {
		ret = epf_infer_setup_doorbell(ctx);
		if (ret) {
			dev_err(&epf->dev, "doorbell setup failed: %d\n", ret);
			return ret;
		}
	}

	for (i = 0; i < INFER_V2_SLOTS; i++) {
		ctx->slots[i].state = INFER_SLOT_EMPTY;
		ctx->slots[i].local_dst = 0;
		ctx->slots[i].capacity = 0;
	}

	WRITE_ONCE(ctx->regs->db_bar, ctx->db_bar);
	WRITE_ONCE(ctx->regs->db_offset, ctx->db_offset);
	WRITE_ONCE(ctx->regs->db_msg, ctx->db_msg);
	WRITE_ONCE(ctx->regs->magic, INFER_MAGIC);
	WRITE_ONCE(ctx->regs->status, INFER_STATUS_IDLE);
	WRITE_ONCE(ctx->regs->ep_flags,
		   INFER_EP_F_ZEROCOPY | INFER_EP_F_MULTI_SLOT);
	epf_infer_update_credit_locked(ctx);
	wmb();

	dev_info(&epf->dev, "reprogram done magic=0x%x db=%u:0x%x v2\n",
		 INFER_MAGIC, ctx->db_bar, ctx->db_offset);
	return 0;
}

static int epf_infer_core_init(struct pci_epf *epf)
{
	return epf_infer_reprogram(epf);
}

static int epf_infer_link_up(struct pci_epf *epf)
{
	dev_info(&epf->dev, "link_up -> reprogram\n");
	return epf_infer_reprogram(epf);
}

static const struct pci_epc_event_ops epf_infer_event_ops = {
	.core_init	= epf_infer_core_init,
	.link_up	= epf_infer_link_up,
};

/* ---- /dev/pci_epf_infer_ctl (reprogram) ---- */

static ssize_t epf_ctl_write(struct file *filp, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char buf[8];
	size_t n = min(count, sizeof(buf) - 1);
	int ret;

	if (!g_epf_infer || !g_epf_infer->epf)
		return -ENODEV;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';
	if (buf[0] != '1')
		return -EINVAL;

	ret = epf_infer_reprogram(g_epf_infer->epf);
	return ret ? ret : count;
}

static const struct file_operations epf_ctl_fops = {
	.owner	= THIS_MODULE,
	.write	= epf_ctl_write,
};

static struct miscdevice epf_ctl_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "pci_epf_infer_ctl",
	.fops	= &epf_ctl_fops,
};

/* ---- /dev/pci_epf_infer0 (ARM / WAIT) ---- */

static int epf_arm_slot(struct epf_infer *ctx, struct infer_ep_recv_reg *reg)
{
	struct epf_infer_slot *slot;
	unsigned long flags;
	dma_addr_t dst;
	size_t cap;

	if (reg->slot >= INFER_V2_SLOTS)
		return -EINVAL;

	if (reg->flags & INFER_EP_REG_F_DMABUF)
		return -EOPNOTSUPP;

	if (reg->flags & INFER_EP_REG_F_STAGING) {
		if (!ctx->buf)
			return -ENODEV;
		dst = ctx->buf_dma;
		cap = ctx->buf_size;
	} else if (reg->flags & INFER_EP_REG_F_ADDR) {
		if (!reg->local_dst || !reg->capacity)
			return -EINVAL;
		dst = (dma_addr_t)reg->local_dst;
		cap = (size_t)reg->capacity;
	} else {
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->lock, flags);
	slot = &ctx->slots[reg->slot];
	if (slot->state == INFER_SLOT_BUSY) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EBUSY;
	}
	slot->local_dst = dst;
	slot->capacity = cap;
	slot->use_staging = !!(reg->flags & INFER_EP_REG_F_STAGING);
	slot->state = INFER_SLOT_POSTED;
	slot->xfer_size = 0;
	epf_infer_update_credit_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);

	dev_dbg(&ctx->epf->dev, "ARM slot=%u dst=%pad cap=%zu staging=%d\n",
		reg->slot, &dst, cap, slot->use_staging);
	return 0;
}

static int epf_wait_slot(struct epf_infer *ctx, struct infer_ep_wait *w)
{
	struct epf_infer_slot *slot;
	unsigned long flags;
	long timeout;
	u64 us = w->timeout_us ? w->timeout_us : 1000000ull;
	int ret;

	if (w->slot >= INFER_V2_SLOTS)
		return -EINVAL;

	timeout = wait_event_interruptible_timeout(
		ctx->slot_wq,
		({
			u32 st;
			spin_lock_irqsave(&ctx->lock, flags);
			st = ctx->slots[w->slot].state;
			spin_unlock_irqrestore(&ctx->lock, flags);
			st == INFER_SLOT_DONE || st == INFER_SLOT_ERROR;
		}),
		usecs_to_jiffies(us));

	if (timeout < 0)
		return timeout;
	if (timeout == 0) {
		w->result = -ETIMEDOUT;
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&ctx->lock, flags);
	slot = &ctx->slots[w->slot];
	if (slot->state == INFER_SLOT_ERROR) {
		w->result = -EIO;
		slot->state = INFER_SLOT_EMPTY;
		ret = -EIO;
	} else if (slot->state == INFER_SLOT_DONE) {
		w->size = slot->xfer_size;
		w->seq = slot->seq;
		w->result = 0;
		slot->state = INFER_SLOT_EMPTY;
		ret = 0;
	} else {
		w->result = -EAGAIN;
		ret = -EAGAIN;
	}
	epf_infer_update_credit_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);
	return ret;
}

static long epf_infer0_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct epf_infer *ctx = filp->private_data;
	void __user *uarg = (void __user *)arg;
	struct infer_ep_recv_reg reg;
	struct infer_ep_post post;
	struct infer_ep_wait wait;
	u32 slot;
	unsigned long flags;
	int ret;

	if (!ctx)
		return -ENODEV;

	switch (cmd) {
	case INFER_EP_IOC_ARM:
		if (copy_from_user(&reg, uarg, sizeof(reg)))
			return -EFAULT;
		return epf_arm_slot(ctx, &reg);

	case INFER_EP_IOC_UNREG:
		if (copy_from_user(&slot, uarg, sizeof(slot)))
			return -EFAULT;
		if (slot >= INFER_V2_SLOTS)
			return -EINVAL;
		spin_lock_irqsave(&ctx->lock, flags);
		if (ctx->slots[slot].state == INFER_SLOT_BUSY) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			return -EBUSY;
		}
		ctx->slots[slot].state = INFER_SLOT_EMPTY;
		ctx->slots[slot].local_dst = 0;
		ctx->slots[slot].capacity = 0;
		epf_infer_update_credit_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		return 0;

	case INFER_EP_IOC_POST:
		if (copy_from_user(&post, uarg, sizeof(post)))
			return -EFAULT;
		if (post.slot >= INFER_V2_SLOTS)
			return -EINVAL;
		spin_lock_irqsave(&ctx->lock, flags);
		if (!ctx->slots[post.slot].local_dst &&
		    !ctx->slots[post.slot].use_staging) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			return -EINVAL;
		}
		if (ctx->slots[post.slot].state == INFER_SLOT_BUSY) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			return -EBUSY;
		}
		ctx->slots[post.slot].state = INFER_SLOT_POSTED;
		epf_infer_update_credit_locked(ctx);
		spin_unlock_irqrestore(&ctx->lock, flags);
		return 0;

	case INFER_EP_IOC_WAIT:
		if (copy_from_user(&wait, uarg, sizeof(wait)))
			return -EFAULT;
		ret = epf_wait_slot(ctx, &wait);
		if (copy_to_user(uarg, &wait, sizeof(wait)))
			return -EFAULT;
		return ret;

	default:
		return -ENOTTY;
	}
}

static int epf_infer0_open(struct inode *inode, struct file *filp)
{
	struct epf_infer *ctx =
		container_of(filp->private_data, struct epf_infer, ep_misc);

	filp->private_data = ctx;
	return 0;
}

static const struct file_operations epf_infer0_fops = {
	.owner		= THIS_MODULE,
	.open		= epf_infer0_open,
	.unlocked_ioctl	= epf_infer0_ioctl,
};

static int epf_infer_bind(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);

	epf->event_ops = &epf_infer_event_ops;
	g_epf_infer = ctx;
	dev_info(&epf->dev,
		 "bind ok; AFTER start: echo 1 > /dev/pci_epf_infer_ctl\n");
	return 0;
}

static void epf_infer_unbind(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;

	if (g_epf_infer == ctx)
		g_epf_infer = NULL;

	cancel_work_sync(&ctx->cmd_work);

	if (ctx->ep_misc_registered) {
		misc_deregister(&ctx->ep_misc);
		ctx->ep_misc_registered = false;
	}

	if (ctx->db_irq >= 0) {
		bst_pcie_ep_db_irq_free(epc, epf->func_no, epf->vfunc_no,
					ctx->db_irq);
		ctx->db_irq = -1;
	}
	if (ctx->regs) {
		pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
				  &epf->bar[INFER_CTRL_BARNO]);
		pci_epf_free_space(epf, ctx->regs, INFER_CTRL_BARNO,
				   PRIMARY_INTERFACE);
		ctx->regs = NULL;
	}
	epf_infer_cleanup_dma(ctx);
	epf->event_ops = NULL;
}

static struct pci_epf_header epf_infer_header = {
	.vendorid	= INFER_VENDOR_ID,
	.deviceid	= INFER_DEVICE_ID,
	.baseclass_code	= PCI_BASE_CLASS_SYSTEM,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static int epf_infer_probe(struct pci_epf *epf,
			   const struct pci_epf_device_id *id)
{
	struct epf_infer *ctx;
	int ret;

	ctx = devm_kzalloc(&epf->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->epf = epf;
	ctx->db_irq = -1;
	spin_lock_init(&ctx->lock);
	init_completion(&ctx->xfer_done);
	init_waitqueue_head(&ctx->slot_wq);
	INIT_WORK(&ctx->cmd_work, epf_infer_cmd_work);
	epf->header = &epf_infer_header;
	epf->event_ops = &epf_infer_event_ops;
	epf_set_drvdata(epf, ctx);

	ctx->ep_misc.minor = MISC_DYNAMIC_MINOR;
	ctx->ep_misc.name = "pci_epf_infer0";
	ctx->ep_misc.fops = &epf_infer0_fops;
	ctx->ep_misc.parent = &epf->dev;
	ret = misc_register(&ctx->ep_misc);
	if (ret) {
		dev_err(&epf->dev, "misc_register pci_epf_infer0 failed: %d\n",
			ret);
		return ret;
	}
	ctx->ep_misc_registered = true;

	dev_info(&epf->dev, "probe: event_ops + /dev/pci_epf_infer0 ready\n");
	return 0;
}

static const struct pci_epf_device_id epf_infer_ids[] = {
	{ .name = "pci_epf_infer" },
	{},
};

static struct pci_epf_ops epf_infer_ops = {
	.bind	= epf_infer_bind,
	.unbind	= epf_infer_unbind,
};

static struct pci_epf_driver epf_infer_driver = {
	.driver.name	= DRV_NAME,
	.ops		= &epf_infer_ops,
	.id_table	= epf_infer_ids,
	.probe		= epf_infer_probe,
	.owner		= THIS_MODULE,
};

static int __init epf_infer_init(void)
{
	int ret;

	ret = misc_register(&epf_ctl_misc);
	if (ret)
		return ret;

	ret = pci_epf_register_driver(&epf_infer_driver);
	if (ret) {
		misc_deregister(&epf_ctl_misc);
		return ret;
	}
	return 0;
}
module_init(epf_infer_init);

static void __exit epf_infer_exit(void)
{
	pci_epf_unregister_driver(&epf_infer_driver);
	misc_deregister(&epf_ctl_misc);
}
module_exit(epf_infer_exit);

MODULE_DESCRIPTION("Min-latency PCIe EP (doorbell + PUSH/ARM zero-copy path)");
MODULE_LICENSE("GPL");
