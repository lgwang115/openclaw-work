// SPDX-License-Identifier: GPL-2.0
/*
 * pci_epf_infer — minimum-latency PCIe EP function (A2000 / C1200)
 *
 * Critical: BAR/header/doorbell must be programmed in epc event .core_init,
 * NOT only in .bind. On this platform `echo 1 > start` re-inits the
 * controller; programming BARs in bind (before start) gets wiped, and the
 * host then only sees hardware-default BAR1/2/4 with no Region 0.
 *
 * Latency path vs pci_epf_test:
 *   doorbell IRQ -> immediate workqueue (NOT 1ms delayed_work)
 *   DMA with preallocated 4MB buffer
 *   completion = status register (RC polls; no MSI on critical path)
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

#include "infer_proto.h"
#include "../../controller/bst/pcie-bst.h"

#define DRV_NAME "pci_epf_infer"

struct epf_infer {
	struct pci_epf		*epf;
	struct infer_regs	*regs;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct dma_chan		*dma_tx;	/* MEM_TO_DEV: EP -> RC */
	struct dma_chan		*dma_rx;	/* DEV_TO_MEM: RC -> EP */
	struct completion	xfer_done;
	enum dma_status		xfer_status;
	/* current in-flight transfer (single outstanding by design) */
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
};

static void epf_infer_dma_cb(void *param)
{
	struct epf_infer *ctx = param;

	ctx->xfer_status = dma_async_is_tx_complete(ctx->cur_chan,
						    ctx->cur_cookie, NULL, NULL);
	complete(&ctx->xfer_done);
}

/*
 * eDMA slave transfer, same as pci_epf_test private-DMA path:
 * remote PCI address goes into dma_slave_config (the eDMA engine on the
 * PCIe controller understands PCI bus addresses); local buffer is the
 * prep_slave_single address. Plain MEMCPY channels must NOT be used here:
 * a system DMA would treat the RC bus address as a local physical address.
 */
static int epf_infer_dma_xfer(struct epf_infer *ctx,
			      enum dma_transfer_direction dir,
			      dma_addr_t remote, size_t size)
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

	desc = dmaengine_prep_slave_single(chan, ctx->buf_dma, size, dir,
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
		/* prevent a late callback touching a finished transfer */
		dmaengine_terminate_sync(chan);
		return -ETIMEDOUT;
	}
	if (ctx->xfer_status != DMA_COMPLETE)
		return -EIO;
	return 0;
}

static void epf_infer_cmd_work(struct work_struct *work)
{
	struct epf_infer *ctx = container_of(work, struct epf_infer, cmd_work);
	struct infer_regs *regs = ctx->regs;
	u32 cmd, size;
	u64 pci_addr;
	int ret = 0;
	unsigned long flags;

	if (!regs)
		goto out;

	cmd = READ_ONCE(regs->command);
	if (!cmd)
		goto out;

	size = READ_ONCE(regs->size);
	pci_addr = READ_ONCE(regs->pci_addr);

	WRITE_ONCE(regs->command, INFER_CMD_NONE);
	WRITE_ONCE(regs->status, INFER_STATUS_BUSY);

	if (!size || size > ctx->buf_size || !ctx->buf) {
		ret = -EINVAL;
		goto done;
	}

	switch (cmd) {
	case INFER_CMD_WRITE:
		ret = epf_infer_dma_xfer(ctx, DMA_DEV_TO_MEM, pci_addr, size);
		break;
	case INFER_CMD_READ:
		ret = epf_infer_dma_xfer(ctx, DMA_MEM_TO_DEV, pci_addr, size);
		break;
	default:
		ret = -EINVAL;
		break;
	}

done:
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

/* Pick eDMA channels that belong to the PCIe controller (pci_epf_test style) */
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
	dev_info(&ctx->epf->dev, "DMA ready, buf %zu @ %pad\n",
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
	enum pci_barno barno = INFER_CTRL_BARNO; /* BAR1: DDR via inbound ATU */
	struct pci_epf_bar *epf_bar = &epf->bar[barno];
	const struct pci_epc_features *features;
	size_t align = PAGE_SIZE;
	int ret;

	/*
	 * Allocate backing memory only once. pci_epf_alloc_space() fills
	 * epf_bar (phys_addr/addr/size/barno). Do NOT memset epf_bar on re-entry.
	 * BAR0 is reserved for MSI-X table + doorbell hardware — do not use it
	 * for protocol registers (host reads of BAR0+0 see HW, not our DDR).
	 */
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

/*
 * Force (re)program header + BAR + doorbell into the live controller.
 * Must run AFTER `echo 1 > .../start` on this BST platform: start wipes
 * BAR/ATU that were programmed during bind.
 */
static int epf_infer_reprogram(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	int ret;

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

	WRITE_ONCE(ctx->regs->db_bar, ctx->db_bar);
	WRITE_ONCE(ctx->regs->db_offset, ctx->db_offset);
	WRITE_ONCE(ctx->regs->db_msg, ctx->db_msg);
	WRITE_ONCE(ctx->regs->magic, INFER_MAGIC);
	WRITE_ONCE(ctx->regs->status, INFER_STATUS_IDLE);
	wmb();

	dev_info(&epf->dev, "reprogram done magic=0x%x db=%u:0x%x\n",
		 INFER_MAGIC, ctx->db_bar, ctx->db_offset);
	return 0;
}

static int epf_infer_core_init(struct pci_epf *epf)
{
	/* If EPC ever calls this after start, treat as reprogram. */
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

/* Global: only one function instance is supported for now. */
static struct epf_infer *g_epf_infer;

/*
 * /dev/pci_epf_infer_ctl — write "1" AFTER controller start to reprogram
 * BAR/doorbell (BST start wipes BAR config done during bind).
 * Avoids hunting for pci-epf sysfs paths.
 */
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

static int epf_infer_bind(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);

	/*
	 * Do NOT program BARs in bind: BST `start` resets the controller and
	 * wipes them. After start: echo 1 > /dev/pci_epf_infer_ctl
	 */
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

	ctx = devm_kzalloc(&epf->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->epf = epf;
	ctx->db_irq = -1;
	spin_lock_init(&ctx->lock);
	init_completion(&ctx->xfer_done);
	INIT_WORK(&ctx->cmd_work, epf_infer_cmd_work);
	epf->header = &epf_infer_header;
	/* Register early so start/core_init can see it (pci_epf_test style). */
	epf->event_ops = &epf_infer_event_ops;
	epf_set_drvdata(epf, ctx);
	dev_info(&epf->dev, "probe: event_ops ready\n");
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

MODULE_DESCRIPTION("Min-latency PCIe EP function (doorbell + status poll)");
MODULE_LICENSE("GPL");
