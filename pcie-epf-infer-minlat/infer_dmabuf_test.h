/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _INFER_DMABUF_TEST_H_
#define _INFER_DMABUF_TEST_H_

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#endif

#define INFER_DMABUF_IOC_MAGIC 'D'

struct infer_dmabuf_export {
	__u64 size;   /* in: requested; out: granted (page-aligned) */
	__s32 fd;     /* out: dma-buf fd */
	__u32 flags;  /* 0 */
};

#define INFER_DMABUF_IOC_EXPORT \
	_IOWR(INFER_DMABUF_IOC_MAGIC, 1, struct infer_dmabuf_export)

#endif
