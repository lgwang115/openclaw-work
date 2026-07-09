/* SPDX-License-Identifier: GPL-2.0 */
/*
 * infer_dmabuf_test — minimal contiguous dma-buf exporter for EP POST_RECV tests
 *
 * /dev/infer_dmabuf_test
 *   INFER_DMABUF_IOC_EXPORT → returns a dma-buf fd (single SG after map)
 *   mmap(fd) works via dma-buf mmap for CPU verify
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>

#include "infer_dmabuf_test.h"

#define DRV_NAME "infer_dmabuf_test"
#define MAX_EXPORT_SIZE (4 * 1024 * 1024)

struct infer_dmabuf_obj {
	void		*vaddr;
	struct page	*page;
	size_t		size;
	int		order;
};

static struct sg_table *infer_dmabuf_map(struct dma_buf_attachment *att,
					 enum dma_data_direction dir)
{
	struct infer_dmabuf_obj *obj = att->dmabuf->priv;
	struct sg_table *sgt;
	int nents;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(sgt, 1, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}

	sg_set_page(sgt->sgl, obj->page, obj->size, 0);
	nents = dma_map_sg(att->dev, sgt->sgl, 1, dir);
	if (nents <= 0) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(nents ? nents : -EIO);
	}
	return sgt;
}

static void infer_dmabuf_unmap(struct dma_buf_attachment *att,
			       struct sg_table *sgt,
			       enum dma_data_direction dir)
{
	dma_unmap_sg(att->dev, sgt->sgl, sgt->nents, dir);
	sg_free_table(sgt);
	kfree(sgt);
}

static void infer_dmabuf_release(struct dma_buf *dmabuf)
{
	struct infer_dmabuf_obj *obj = dmabuf->priv;

	if (obj->vaddr)
		free_pages_exact(obj->vaddr, obj->size);
	kfree(obj);
}

static int infer_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct infer_dmabuf_obj *obj = dmabuf->priv;
	unsigned long pfn = page_to_pfn(obj->page);
	size_t size = vma->vm_end - vma->vm_start;

	if (size > obj->size)
		return -EINVAL;
	return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}

static const struct dma_buf_ops infer_dmabuf_ops = {
	.cache_sgt_mapping = true,
	.map_dma_buf	= infer_dmabuf_map,
	.unmap_dma_buf	= infer_dmabuf_unmap,
	.release	= infer_dmabuf_release,
	.mmap		= infer_dmabuf_mmap,
};

static int infer_dmabuf_export_fd(size_t size, int *fd_out)
{
	struct infer_dmabuf_obj *obj;
	DEFINE_DMA_BUF_EXPORT_INFO(exp);
	struct dma_buf *dmabuf;
	int fd;

	if (!size || size > MAX_EXPORT_SIZE)
		return -EINVAL;
	size = PAGE_ALIGN(size);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->vaddr = alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO);
	if (!obj->vaddr) {
		kfree(obj);
		return -ENOMEM;
	}
	obj->page = virt_to_page(obj->vaddr);
	obj->size = size;

	exp.ops = &infer_dmabuf_ops;
	exp.size = size;
	exp.flags = O_RDWR | O_CLOEXEC;
	exp.priv = obj;
	exp.exp_name = DRV_NAME;

	dmabuf = dma_buf_export(&exp);
	if (IS_ERR(dmabuf)) {
		free_pages_exact(obj->vaddr, size);
		kfree(obj);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}
	*fd_out = fd;
	return 0;
}

static long infer_dmabuf_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct infer_dmabuf_export req;
	void __user *uarg = (void __user *)arg;
	int ret, fd;

	if (cmd != INFER_DMABUF_IOC_EXPORT)
		return -ENOTTY;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	ret = infer_dmabuf_export_fd(req.size, &fd);
	if (ret)
		return ret;

	req.fd = fd;
	req.size = PAGE_ALIGN(req.size);
	if (copy_to_user(uarg, &req, sizeof(req))) {
		/* userspace must close fd; best-effort */
		return -EFAULT;
	}
	return 0;
}

static const struct file_operations infer_dmabuf_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= infer_dmabuf_ioctl,
};

static struct miscdevice infer_dmabuf_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "infer_dmabuf_test",
	.fops	= &infer_dmabuf_fops,
};

static int __init infer_dmabuf_init(void)
{
	return misc_register(&infer_dmabuf_misc);
}
module_init(infer_dmabuf_init);

static void __exit infer_dmabuf_exit(void)
{
	misc_deregister(&infer_dmabuf_misc);
}
module_exit(infer_dmabuf_exit);

MODULE_DESCRIPTION("dma-buf exporter for pci_epf_infer POST_RECV tests");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
