// SPDX-License-Identifier: GPL-2.0
/*
 * pci_epf_infer — minimum-latency PCIe EP function (A2000 / C1200)
 *
 * Critical: BAR/header/doorbell must be programmed AFTER `echo 1 > start`
 * via /dev/pci_epf_infer_ctl (BST start wipes bind-time BAR/ATU).
 *
 * v1: WRITE/READ against driver staging buffer (inferlat).
 * v2: PUSH → eDMA remote pci_addr → per-slot local_dst (POST_RECV each packet).
 *
 * Latency path: doorbell IRQ submits eDMA immediately (no workqueue),
 * DMA callback writes status. Single-outstanding via ctx->busy.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/scatterlist.h>
#include <linux/ktime.h>

#include "infer_proto_v2.h"
#include "../../controller/bst/pcie-bst.h"

#define DRV_NAME "pci_epf_infer"

/*
 * Per-N-transfer summary of eDMA submit→callback timing. 0 = off (default).
 * Keep the hot path printk-free: only one dev_info per dma_log_every DMAs.
 * Exact counters are always readable via INFER_EP_IOC_DMA_STATS.
 */
static unsigned int dma_log_every;
module_param(dma_log_every, uint, 0644);
MODULE_PARM_DESC(dma_log_every,
	"log an eDMA timing summary every N completed transfers (0=off)");

/*
 * Diagnostic: after issuing the eDMA, bounded busy-poll dma_async_is_tx_complete
 * in the doorbell handler to timestamp when the HARDWARE finished, without
 * changing the completion path (the async callback still writes status). Lets us
 * split issue→callback into "issue→HW done" vs "HW done→callback dispatch".
 * 0 = off (default). Value = max microseconds to poll before giving up.
 */
static unsigned int dma_poll_diag;
module_param(dma_poll_diag, uint, 0644);
MODULE_PARM_DESC(dma_poll_diag,
	"diagnostic: poll HW completion up to N us to time issue->HW-done (0=off)");

struct epf_infer_slot {
	dma_addr_t		local_dst;
	size_t			capacity;
	u32			state;
	u32			xfer_size;
	u32			seq;
	bool			use_staging;
	/* optional dma-buf backing (released on WAIT/UNREG/re-POST) */
	struct dma_buf		*dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table		*sgt;
};

struct epf_infer {
	struct pci_epf		*epf;
	struct infer_regs_v2	*regs;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct dma_chan		*dma_tx;
	struct dma_chan		*dma_rx;
	struct dma_chan		*cur_chan;
	dma_cookie_t		cur_cookie;
	int			db_irq;
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	spinlock_t		lock;
	bool			busy;
	bool			dma_ok;
	/* in-flight command context (valid while busy) */
	u32			pend_cmd;
	u32			pend_slot;
	u32			pend_size;
	/* eDMA timing stats (submit → completion callback), protected by lock */
	u64			dma_t0_ns;    /* issue timestamp of in-flight DMA */
	u64			ts_h_ns;      /* doorbell handler entry (accepted cmd) */
	u64			ts_setup_ns;  /* dma_submit entry (before slave_config) */
	u64			dma_hwdone_ns; /* diag: HW completion detected by polling */
	size_t			dma_cur_bytes;
	u64			dma_count;
	u64			dma_sum_ns;
	u64			dma_min_ns;
	u64			dma_max_ns;
	u64			dma_last_ns;
	u64			dma_bytes;
	/* per-segment breakdown (same dma_count) */
	u64			prologue_sum_ns, prologue_min_ns, prologue_max_ns;
	u64			setup_sum_ns, setup_min_ns, setup_max_ns;
	u64			total_sum_ns, total_min_ns, total_max_ns;
	/* diag: issue → HW-done (polled); count may be < dma_count */
	u64			hwdone_count, hwdone_sum_ns, hwdone_min_ns, hwdone_max_ns;
	struct epf_infer_slot	slots[INFER_V2_SLOTS];
	wait_queue_head_t	slot_wq;
	struct miscdevice	ep_misc;
	bool			ep_misc_registered;
};

static struct epf_infer *g_epf_infer;

static void epf_slot_release_dmabuf(struct epf_infer_slot *slot)
{
	if (slot->sgt && slot->attach) {
		dma_buf_unmap_attachment(slot->attach, slot->sgt, DMA_FROM_DEVICE);
		slot->sgt = NULL;
	}
	if (slot->attach && slot->dmabuf) {
		dma_buf_detach(slot->dmabuf, slot->attach);
		slot->attach = NULL;
	}
	if (slot->dmabuf) {
		dma_buf_put(slot->dmabuf);
		slot->dmabuf = NULL;
	}
}

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

static void epf_infer_dma_cb(void *param);

/*
 * Submit eDMA only (no wait). Safe from doorbell IRQ.
 * Completion is handled in epf_infer_dma_cb.
 */
static int epf_infer_dma_submit(struct epf_infer *ctx,
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

	/* segment start: slave_config + prep + submit (before t0/issue) */
	ctx->ts_setup_ns = ktime_get_ns();

	sconf.direction = dir;
	if (dir == DMA_MEM_TO_DEV)
		sconf.dst_addr = remote;
	else
		sconf.src_addr = remote;

	ret = dmaengine_slave_config(chan, &sconf);
	if (ret)
		return ret;

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
	/*
	 * Timestamp right before issuing so the interval captures the eDMA
	 * engine + transfer + completion-callback dispatch. Single-outstanding
	 * (ctx->busy) means no other DMA can overwrite these before the cb.
	 */
	ctx->dma_cur_bytes = size;
	ctx->dma_t0_ns = ktime_get_ns();
	dma_async_issue_pending(chan);
	return 0;
}

#define EPF_ACC(sum, mn, mx, first, dt) do {         \
	if ((first) || (dt) < (mn))                  \
		(mn) = (dt);                         \
	if ((dt) > (mx))                             \
		(mx) = (dt);                         \
	(sum) += (dt);                               \
} while (0)

/*
 * Accumulate one transfer's timing. Caller holds ctx->lock, runs in the DMA
 * completion callback. Splits EP-internal time into segments:
 *   prologue = handler entry → dma_submit entry  (command parse + lock)
 *   setup    = slave_config + prep + submit       (ts_setup → issue/t0)
 *   xfer     = issue → this callback              (eDMA + completion dispatch)
 *   total    = handler entry → this callback      (EP internal round)
 */
static void epf_infer_dma_account_locked(struct epf_infer *ctx)
{
	u64 now, dt;
	bool first;

	if (!ctx->dma_t0_ns)
		return;
	now = ktime_get_ns();
	first = (ctx->dma_count == 0);

	/* xfer: issue → callback */
	dt = now - ctx->dma_t0_ns;
	EPF_ACC(ctx->dma_sum_ns, ctx->dma_min_ns, ctx->dma_max_ns, first, dt);
	ctx->dma_last_ns = dt;

	/* setup: ts_setup → issue */
	if (ctx->ts_setup_ns && ctx->dma_t0_ns > ctx->ts_setup_ns) {
		dt = ctx->dma_t0_ns - ctx->ts_setup_ns;
		EPF_ACC(ctx->setup_sum_ns, ctx->setup_min_ns,
			ctx->setup_max_ns, first, dt);
	}

	/* prologue: handler entry → ts_setup */
	if (ctx->ts_h_ns && ctx->ts_setup_ns > ctx->ts_h_ns) {
		dt = ctx->ts_setup_ns - ctx->ts_h_ns;
		EPF_ACC(ctx->prologue_sum_ns, ctx->prologue_min_ns,
			ctx->prologue_max_ns, first, dt);
	}

	/* total: handler entry → callback */
	if (ctx->ts_h_ns && now > ctx->ts_h_ns) {
		dt = now - ctx->ts_h_ns;
		EPF_ACC(ctx->total_sum_ns, ctx->total_min_ns,
			ctx->total_max_ns, first, dt);
	}

	/* diag: issue → HW-done (polled). Own count (may miss on races). */
	if (ctx->dma_hwdone_ns && ctx->dma_hwdone_ns >= ctx->dma_t0_ns) {
		bool hfirst = (ctx->hwdone_count == 0);

		dt = ctx->dma_hwdone_ns - ctx->dma_t0_ns;
		EPF_ACC(ctx->hwdone_sum_ns, ctx->hwdone_min_ns,
			ctx->hwdone_max_ns, hfirst, dt);
		ctx->hwdone_count++;
	}

	ctx->dma_bytes += ctx->dma_cur_bytes;
	ctx->dma_count++;
	ctx->dma_t0_ns = 0;
	ctx->ts_h_ns = 0;
	ctx->ts_setup_ns = 0;
	ctx->dma_hwdone_ns = 0;

	if (dma_log_every && (ctx->dma_count % dma_log_every) == 0) {
		u64 n = ctx->dma_count;

		dev_info(&ctx->epf->dev,
			 "eDMA n=%llu avg(ns): prologue=%llu setup=%llu xfer=%llu total=%llu\n",
			 n, ctx->prologue_sum_ns / n, ctx->setup_sum_ns / n,
			 ctx->dma_sum_ns / n, ctx->total_sum_ns / n);
	}
}

static void epf_infer_finish_locked(struct epf_infer *ctx, int ret)
{
	struct infer_regs_v2 *regs = ctx->regs;
	struct epf_infer_slot *slot;
	u32 seq;

	if (ctx->pend_cmd == INFER_CMD_PUSH &&
	    ctx->pend_slot < INFER_V2_SLOTS) {
		slot = &ctx->slots[ctx->pend_slot];
		if (ret) {
			slot->state = INFER_SLOT_ERROR;
		} else {
			seq = READ_ONCE(regs->seq) + 1;
			WRITE_ONCE(regs->seq, seq);
			slot->xfer_size = ctx->pend_size;
			slot->seq = seq;
			slot->state = INFER_SLOT_DONE;
		}
		epf_infer_update_credit_locked(ctx);
	}

	wmb();
	if (regs)
		WRITE_ONCE(regs->status, ret ? INFER_STATUS_FAIL : INFER_STATUS_OK);

	ctx->pend_cmd = INFER_CMD_NONE;
	ctx->busy = false;
}

static void epf_infer_dma_cb(void *param)
{
	struct epf_infer *ctx = param;
	enum dma_status st;
	unsigned long flags;
	int ret = 0;

	st = dma_async_is_tx_complete(ctx->cur_chan, ctx->cur_cookie, NULL, NULL);
	if (st != DMA_COMPLETE)
		ret = -EIO;

	spin_lock_irqsave(&ctx->lock, flags);
	epf_infer_dma_account_locked(ctx);
	epf_infer_finish_locked(ctx, ret);
	spin_unlock_irqrestore(&ctx->lock, flags);

	wake_up_interruptible(&ctx->slot_wq);
}

/*
 * Doorbell IRQ: single-outstanding kick. Parse command and submit eDMA
 * immediately — no workqueue scheduling on the critical path.
 */
static int epf_infer_doorbell_handler(int irq, void *arg)
{
	struct epf_infer *ctx = arg;
	struct infer_regs_v2 *regs = ctx->regs;
	struct epf_infer_slot *slot;
	unsigned long flags;
	u32 cmd, size, slot_idx;
	u64 pci_addr;
	u64 t_h = ktime_get_ns();  /* handler entry (before doorbell→handler is unmeasurable) */
	dma_addr_t local;
	enum dma_transfer_direction dir;
	int ret;

	if (!regs)
		return IRQ_HANDLED;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->busy) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return IRQ_HANDLED;
	}

	cmd = READ_ONCE(regs->command);
	if (!cmd) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return IRQ_HANDLED;
	}

	size = READ_ONCE(regs->size);
	pci_addr = READ_ONCE(regs->pci_addr);
	slot_idx = READ_ONCE(regs->slot);

	WRITE_ONCE(regs->command, INFER_CMD_NONE);
	WRITE_ONCE(regs->status, INFER_STATUS_BUSY);

	ctx->busy = true;
	ctx->pend_cmd = cmd;
	ctx->pend_slot = slot_idx;
	ctx->pend_size = size;
	ctx->ts_h_ns = t_h;

	switch (cmd) {
	case INFER_CMD_WRITE:
		if (!size || size > ctx->buf_size || !ctx->buf) {
			epf_infer_finish_locked(ctx, -EINVAL);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return IRQ_HANDLED;
		}
		local = ctx->buf_dma;
		dir = DMA_DEV_TO_MEM;
		break;

	case INFER_CMD_READ:
		if (!size || size > ctx->buf_size || !ctx->buf) {
			epf_infer_finish_locked(ctx, -EINVAL);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return IRQ_HANDLED;
		}
		local = ctx->buf_dma;
		dir = DMA_MEM_TO_DEV;
		break;

	case INFER_CMD_PUSH:
		if (slot_idx >= INFER_V2_SLOTS || !size) {
			epf_infer_finish_locked(ctx, -EINVAL);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return IRQ_HANDLED;
		}
		slot = &ctx->slots[slot_idx];
		if (slot->state != INFER_SLOT_POSTED) {
			epf_infer_finish_locked(ctx, -EINVAL);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return IRQ_HANDLED;
		}
		if (size > slot->capacity) {
			epf_infer_finish_locked(ctx, -EMSGSIZE);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return IRQ_HANDLED;
		}
		local = slot->local_dst;
		slot->state = INFER_SLOT_BUSY;
		epf_infer_update_credit_locked(ctx);
		dir = DMA_DEV_TO_MEM;
		break;

	default:
		epf_infer_finish_locked(ctx, -EINVAL);
		spin_unlock_irqrestore(&ctx->lock, flags);
		return IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	ret = epf_infer_dma_submit(ctx, dir, pci_addr, local, size);
	if (ret) {
		spin_lock_irqsave(&ctx->lock, flags);
		epf_infer_finish_locked(ctx, ret);
		spin_unlock_irqrestore(&ctx->lock, flags);
		wake_up_interruptible(&ctx->slot_wq);
		return IRQ_HANDLED;
	}

	/*
	 * Diagnostic only: bounded busy-poll to timestamp when the HW finishes.
	 * Does not touch the completion path — the async callback still runs and
	 * writes status. Reads HW status directly, so ts is accurate even though
	 * the completion IRQ may be deferred behind this handler.
	 */
	if (dma_poll_diag) {
		u64 t0 = ktime_get_ns();
		u64 budget = (u64)dma_poll_diag * 1000ull;

		do {
			if (dma_async_is_tx_complete(ctx->cur_chan,
						     ctx->cur_cookie,
						     NULL, NULL) == DMA_COMPLETE) {
				ctx->dma_hwdone_ns = ktime_get_ns();
				break;
			}
			cpu_relax();
		} while (ktime_get_ns() - t0 < budget);
	}
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
	ctx->regs->ep_flags = INFER_EP_F_ZEROCOPY | INFER_EP_F_MULTI_SLOT |
			      INFER_EP_F_DMABUF;

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
		epf_slot_release_dmabuf(&ctx->slots[i]);
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
		   INFER_EP_F_ZEROCOPY | INFER_EP_F_MULTI_SLOT |
		   INFER_EP_F_DMABUF);
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

/* ---- /dev/pci_epf_infer0 (POST_RECV / WAIT) ---- */

static int epf_post_recv_dmabuf(struct epf_infer *ctx,
				struct infer_ep_recv_reg *reg,
				dma_addr_t *dst_out, size_t *cap_out,
				struct dma_buf **db_out,
				struct dma_buf_attachment **att_out,
				struct sg_table **sgt_out)
{
	struct device *dma_dev;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t base;
	size_t dma_len, avail;

	if (reg->dmabuf_fd < 0)
		return -EINVAL;
	if (!ctx->epf || !ctx->epf->epc)
		return -ENODEV;

	dma_dev = ctx->epf->epc->dev.parent;
	dmabuf = dma_buf_get(reg->dmabuf_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	if (reg->dmabuf_offset >= dmabuf->size) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	attach = dma_buf_attach(dmabuf, dma_dev);
	if (IS_ERR(attach)) {
		dma_buf_put(dmabuf);
		return PTR_ERR(attach);
	}

	sgt = dma_buf_map_attachment(attach, DMA_FROM_DEVICE);
	if (IS_ERR(sgt)) {
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return PTR_ERR(sgt);
	}

	/* prep_slave_single needs one contiguous DMA address */
	if (sgt->nents != 1) {
		dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	base = sg_dma_address(sgt->sgl);
	dma_len = sg_dma_len(sgt->sgl);
	if (reg->dmabuf_offset >= dma_len) {
		dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	avail = dma_len - reg->dmabuf_offset;
	if (reg->capacity && reg->capacity < avail)
		avail = reg->capacity;

	*dst_out = base + reg->dmabuf_offset;
	*cap_out = avail;
	*db_out = dmabuf;
	*att_out = attach;
	*sgt_out = sgt;
	return 0;
}

static int epf_post_recv_slot(struct epf_infer *ctx, struct infer_ep_recv_reg *reg)
{
	struct epf_infer_slot *slot;
	struct epf_infer_slot old = {};
	unsigned long flags;
	dma_addr_t dst = 0;
	size_t cap = 0;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	bool use_staging = false;
	int ret;

	if (reg->slot >= INFER_V2_SLOTS)
		return -EINVAL;

	if (reg->flags & INFER_EP_REG_F_DMABUF) {
		ret = epf_post_recv_dmabuf(ctx, reg, &dst, &cap,
					   &dmabuf, &attach, &sgt);
		if (ret)
			return ret;
	} else if (reg->flags & INFER_EP_REG_F_STAGING) {
		if (!ctx->buf)
			return -ENODEV;
		dst = ctx->buf_dma;
		cap = ctx->buf_size;
		if (reg->capacity && reg->capacity < cap)
			cap = reg->capacity;
		use_staging = true;
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
		if (dmabuf) {
			dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
		}
		return -EBUSY;
	}
	/* stash previous dmabuf to release outside the lock */
	old.dmabuf = slot->dmabuf;
	old.attach = slot->attach;
	old.sgt = slot->sgt;

	slot->local_dst = dst;
	slot->capacity = cap;
	slot->use_staging = use_staging;
	slot->dmabuf = dmabuf;
	slot->attach = attach;
	slot->sgt = sgt;
	slot->state = INFER_SLOT_POSTED;
	slot->xfer_size = 0;
	epf_infer_update_credit_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);

	epf_slot_release_dmabuf(&old);

	dev_dbg(&ctx->epf->dev,
		"POST_RECV slot=%u dst=%pad cap=%zu staging=%d dmabuf=%d\n",
		reg->slot, &dst, cap, use_staging, !!dmabuf);
	return 0;
}

static int epf_wait_slot(struct epf_infer *ctx, struct infer_ep_wait *w)
{
	struct epf_infer_slot *slot;
	struct epf_infer_slot release = {};
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
		ret = -EIO;
	} else if (slot->state == INFER_SLOT_DONE) {
		w->size = slot->xfer_size;
		w->seq = slot->seq;
		w->result = 0;
		ret = 0;
	} else {
		w->result = -EAGAIN;
		ret = -EAGAIN;
	}
	if (ret != -EAGAIN) {
		release.dmabuf = slot->dmabuf;
		release.attach = slot->attach;
		release.sgt = slot->sgt;
		slot->dmabuf = NULL;
		slot->attach = NULL;
		slot->sgt = NULL;
		slot->state = INFER_SLOT_EMPTY;
		slot->local_dst = 0;
		slot->capacity = 0;
	}
	epf_infer_update_credit_locked(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);

	epf_slot_release_dmabuf(&release);
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
	case INFER_EP_IOC_POST_RECV:
		if (copy_from_user(&reg, uarg, sizeof(reg)))
			return -EFAULT;
		return epf_post_recv_slot(ctx, &reg);

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
		{
			struct epf_infer_slot release = {};

			release.dmabuf = ctx->slots[slot].dmabuf;
			release.attach = ctx->slots[slot].attach;
			release.sgt = ctx->slots[slot].sgt;
			ctx->slots[slot].dmabuf = NULL;
			ctx->slots[slot].attach = NULL;
			ctx->slots[slot].sgt = NULL;
			ctx->slots[slot].state = INFER_SLOT_EMPTY;
			ctx->slots[slot].local_dst = 0;
			ctx->slots[slot].capacity = 0;
			epf_infer_update_credit_locked(ctx);
			spin_unlock_irqrestore(&ctx->lock, flags);
			epf_slot_release_dmabuf(&release);
		}
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

	case INFER_EP_IOC_GET_INFO: {
		struct infer_ep_info info = {};

		info.staging_size = ctx->buf_size;
		info.staging_dma = ctx->buf_dma;
		info.n_slots = INFER_V2_SLOTS;
		if (ctx->regs)
			info.ep_flags = READ_ONCE(ctx->regs->ep_flags);
		else
			info.ep_flags = INFER_EP_F_ZEROCOPY |
					INFER_EP_F_MULTI_SLOT |
					INFER_EP_F_DMABUF;
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case INFER_EP_IOC_DMA_STATS: {
		struct infer_ep_dma_stats stats = {};
		u32 reset;

		if (copy_from_user(&reset, uarg, sizeof(reset)))
			return -EFAULT;

		spin_lock_irqsave(&ctx->lock, flags);
		stats.count = ctx->dma_count;
		stats.sum_ns = ctx->dma_sum_ns;
		stats.min_ns = ctx->dma_count ? ctx->dma_min_ns : 0;
		stats.max_ns = ctx->dma_max_ns;
		stats.last_ns = ctx->dma_last_ns;
		stats.bytes = ctx->dma_bytes;
		stats.prologue_sum_ns = ctx->prologue_sum_ns;
		stats.prologue_min_ns = ctx->dma_count ? ctx->prologue_min_ns : 0;
		stats.prologue_max_ns = ctx->prologue_max_ns;
		stats.setup_sum_ns = ctx->setup_sum_ns;
		stats.setup_min_ns = ctx->dma_count ? ctx->setup_min_ns : 0;
		stats.setup_max_ns = ctx->setup_max_ns;
		stats.total_sum_ns = ctx->total_sum_ns;
		stats.total_min_ns = ctx->dma_count ? ctx->total_min_ns : 0;
		stats.total_max_ns = ctx->total_max_ns;
		stats.hwdone_count = ctx->hwdone_count;
		stats.hwdone_sum_ns = ctx->hwdone_sum_ns;
		stats.hwdone_min_ns = ctx->hwdone_count ? ctx->hwdone_min_ns : 0;
		stats.hwdone_max_ns = ctx->hwdone_max_ns;
		if (reset) {
			ctx->dma_count = 0;
			ctx->dma_sum_ns = 0;
			ctx->dma_min_ns = 0;
			ctx->dma_max_ns = 0;
			ctx->dma_last_ns = 0;
			ctx->dma_bytes = 0;
			ctx->prologue_sum_ns = 0;
			ctx->prologue_min_ns = 0;
			ctx->prologue_max_ns = 0;
			ctx->setup_sum_ns = 0;
			ctx->setup_min_ns = 0;
			ctx->setup_max_ns = 0;
			ctx->total_sum_ns = 0;
			ctx->total_min_ns = 0;
			ctx->total_max_ns = 0;
			ctx->hwdone_count = 0;
			ctx->hwdone_sum_ns = 0;
			ctx->hwdone_min_ns = 0;
			ctx->hwdone_max_ns = 0;
		}
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (copy_to_user(uarg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}

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

static int epf_infer0_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct epf_infer *ctx = filp->private_data;
	struct device *dma_dev;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!ctx || !ctx->buf || !ctx->epf || !ctx->epf->epc)
		return -ENODEV;
	if (size > ctx->buf_size)
		return -EINVAL;

	dma_dev = ctx->epf->epc->dev.parent;
	return dma_mmap_coherent(dma_dev, vma, ctx->buf, ctx->buf_dma, size);
}

static const struct file_operations epf_infer0_fops = {
	.owner		= THIS_MODULE,
	.open		= epf_infer0_open,
	.unlocked_ioctl	= epf_infer0_ioctl,
	.mmap		= epf_infer0_mmap,
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
	int i;

	if (g_epf_infer == ctx)
		g_epf_infer = NULL;

	/* Drain any in-flight DMA before tearing down. */
	if (ctx->cur_chan && ctx->busy)
		dmaengine_terminate_sync(ctx->cur_chan);

	for (i = 0; i < INFER_V2_SLOTS; i++)
		epf_slot_release_dmabuf(&ctx->slots[i]);

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
	init_waitqueue_head(&ctx->slot_wq);
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

MODULE_DESCRIPTION("Min-latency PCIe EP (doorbell + PUSH/POST_RECV zero-copy path)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
