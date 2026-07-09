// SPDX-License-Identifier: GPL-2.0
/*
 * pci_epf_infer — minimum-latency PCIe EP function (A2000 / C1200)
 *
 * Latency path vs pci_epf_test:
 *   doorbell IRQ -> immediate workqueue (NOT 1ms delayed_work)
 *   DMA with preallocated 4MB buffer (no per-xfer alloc / CRC / random)
 *   completion = status register write (RC polls; no MSI on critical path)
 *
 * Build: see Makefile (out-of-tree). Needs kernel tree with pcie-bst.h.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>

#include "infer_proto.h"
/*
 * Use the same BST header path as pci-epf-test.c when built in-tree:
 *   drivers/pci/endpoint/functions/pci_epf_infer.c
 * Out-of-tree builds should use install-into-kernel.sh (recommended),
 * because bst_pcie_ep_db_* are typically not EXPORT_SYMBOL'd.
 */
#include "../../controller/bst/pcie-bst.h"

#define DRV_NAME "pci_epf_infer"

struct epf_infer {
	struct pci_epf		*epf;
	struct infer_regs	*regs;
	void			*buf;
	dma_addr_t		buf_dma;
	size_t			buf_size;
	struct dma_chan		*dma_tx;
	struct dma_chan		*dma_rx;
	struct completion	xfer_done;
	enum dma_status		xfer_status;
	struct work_struct	cmd_work;
	int			db_irq;
	u32			db_bar;
	u32			db_offset;
	u32			db_msg;
	spinlock_t		lock;
	bool			busy;
};

struct epf_dma_cb {
	struct epf_infer	*ctx;
	struct dma_chan		*chan;
	dma_cookie_t		cookie;
};

static void epf_infer_dma_cb(void *param)
{
	struct epf_dma_cb *cb = param;

	cb->ctx->xfer_status = dma_async_is_tx_complete(cb->chan, cb->cookie,
							NULL, NULL);
	complete(&cb->ctx->xfer_done);
}

static int epf_infer_dma_xfer(struct epf_infer *ctx,
			      enum dma_transfer_direction dir,
			      dma_addr_t remote, size_t size)
{
	struct dma_chan *chan = (dir == DMA_DEV_TO_MEM) ? ctx->dma_rx : ctx->dma_tx;
	struct dma_async_tx_descriptor *desc;
	struct epf_dma_cb cb;
	dma_addr_t local = ctx->buf_dma;
	dma_cookie_t cookie;
	dma_addr_t dst, src;

	if (!chan)
		return -ENODEV;

	if (dir == DMA_DEV_TO_MEM) {
		dst = local;
		src = remote;
	} else {
		dst = remote;
		src = local;
	}

	reinit_completion(&ctx->xfer_done);
	ctx->xfer_status = DMA_IN_PROGRESS;

	desc = dmaengine_prep_dma_memcpy(chan, dst, src, size,
					 DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc)
		return -EIO;

	cb.ctx = ctx;
	cb.chan = chan;
	desc->callback = epf_infer_dma_cb;
	desc->callback_param = &cb;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return -EIO;
	cb.cookie = cookie;
	dma_async_issue_pending(chan);

	if (!wait_for_completion_timeout(&ctx->xfer_done, msecs_to_jiffies(100)))
		return -ETIMEDOUT;
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

	/* Hardirq: only schedule immediate work (may not sleep here). */
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

static int epf_infer_setup_dma(struct epf_infer *ctx)
{
	struct device *dma_dev = ctx->epf->epc->dev.parent;
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	ctx->dma_tx = dma_request_channel(mask, NULL, NULL);
	ctx->dma_rx = dma_request_channel(mask, NULL, NULL);
	if (!ctx->dma_tx || !ctx->dma_rx) {
		dev_err(&ctx->epf->dev, "failed to get DMA channels (tx=%p rx=%p)\n",
			ctx->dma_tx, ctx->dma_rx);
		return -ENODEV;
	}

	ctx->buf_size = INFER_MAX_XFER;
	ctx->buf = dma_alloc_coherent(dma_dev, ctx->buf_size, &ctx->buf_dma,
				      GFP_KERNEL);
	if (!ctx->buf)
		return -ENOMEM;

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

static int epf_infer_set_bar(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar = &epf->bar[BAR_0];
	size_t align = epc->mem ? epc->mem->window.page_size : PAGE_SIZE;
	void *base;
	int ret;

	epf_bar->flags = PCI_BASE_ADDRESS_SPACE_MEMORY |
			 PCI_BASE_ADDRESS_MEM_TYPE_32;
	epf_bar->barno = BAR_0;
	epf_bar->size = INFER_BAR0_SIZE;

	base = pci_epf_alloc_space(epf, INFER_BAR0_SIZE, BAR_0, align,
				   PRIMARY_INTERFACE);
	if (!base)
		return -ENOMEM;

	ctx->regs = base;
	memset(ctx->regs, 0, sizeof(*ctx->regs));
	ctx->regs->magic = INFER_MAGIC;
	ctx->regs->status = INFER_STATUS_IDLE;

	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
	if (ret) {
		pci_epf_free_space(epf, base, BAR_0, PRIMARY_INTERFACE);
		ctx->regs = NULL;
	}
	return ret;
}

static int epf_infer_bind(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	int ret;

	ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, epf->header);
	if (ret)
		return ret;

	ret = epf_infer_set_bar(epf);
	if (ret)
		return ret;

	ret = epf_infer_setup_dma(ctx);
	if (ret)
		dev_err(&epf->dev, "DMA setup failed: %d (cmds will fail)\n", ret);

	ret = epf_infer_setup_doorbell(ctx);
	if (ret) {
		dev_err(&epf->dev, "doorbell setup failed: %d\n", ret);
		return ret;
	}

	WRITE_ONCE(ctx->regs->db_bar, ctx->db_bar);
	WRITE_ONCE(ctx->regs->db_offset, ctx->db_offset);
	WRITE_ONCE(ctx->regs->db_msg, ctx->db_msg);
	return 0;
}

static void epf_infer_unbind(struct pci_epf *epf)
{
	struct epf_infer *ctx = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;

	cancel_work_sync(&ctx->cmd_work);

	if (ctx->db_irq >= 0) {
		bst_pcie_ep_db_irq_free(epc, epf->func_no, epf->vfunc_no,
					ctx->db_irq);
		ctx->db_irq = -1;
	}
	if (ctx->regs) {
		pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, &epf->bar[BAR_0]);
		pci_epf_free_space(epf, ctx->regs, BAR_0, PRIMARY_INTERFACE);
		ctx->regs = NULL;
	}
	epf_infer_cleanup_dma(ctx);
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
	epf_set_drvdata(epf, ctx);
	return 0;
}

static const struct pci_epf_device_id epf_infer_ids[] = {
	{ .name = "pci_epf_infer" },
	{},
};

/* Not const: pci_epf_driver.ops is struct pci_epf_ops * on this kernel */
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
	return pci_epf_register_driver(&epf_infer_driver);
}
module_init(epf_infer_init);

static void __exit epf_infer_exit(void)
{
	pci_epf_unregister_driver(&epf_infer_driver);
}
module_exit(epf_infer_exit);

MODULE_DESCRIPTION("Min-latency PCIe EP function (doorbell + status poll)");
MODULE_LICENSE("GPL");
