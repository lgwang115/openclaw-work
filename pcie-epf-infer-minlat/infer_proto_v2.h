/* SPDX-License-Identifier: GPL-2.0 */
/*
 * infer_proto_v2.h — zero-copy NPU interconnect protocol
 *
 * Wired into pci_epf_infer / infer_rc (P1/P2):
 *   EP: POST_RECV/WAIT + per-transfer local_dst for PUSH
 *   RC: INFER_IOC_PUSH with external pci_addr
 * v1 INFER_IOC_XFER (staging) remains for inferlat regression.
 *
 * Design: ZEROCOPY.md
 */
#ifndef _INFER_PROTO_V2_H_
#define _INFER_PROTO_V2_H_

#include "infer_proto.h"

/* ---- commands (BAR1 command field) ---- */
#define INFER_CMD_PUSH              3  /* RC→EP: DMA remote pci_addr → EP local_dst[slot] */

/* ---- slot / credit ---- */
#define INFER_V2_SLOTS              2
#define INFER_SLOT_EMPTY            0
#define INFER_SLOT_POSTED           1
#define INFER_SLOT_BUSY             2
#define INFER_SLOT_DONE             3
#define INFER_SLOT_ERROR            4

/*
 * BAR1 layout for v2 (little-endian).
 * First fields match v1 infer_regs so old hosts still see magic/db_*.
 */
struct infer_regs_v2 {
	/* --- v1 compatible prefix --- */
	__u32 magic;
	__u32 command;        /* NONE / WRITE / READ / PUSH */
	__u32 status;         /* IDLE / OK / FAIL / BUSY */
	__u32 size;
	__u64 pci_addr;       /* remote (RC-side) bus address for EP eDMA */
	__u32 db_bar;
	__u32 db_offset;
	__u32 db_msg;
	__u32 seq;            /* EP increments on each completed xfer */

	/* --- v2 --- */
	__u32 slot;           /* RC→EP: target recv slot */
	__u32 posted_mask;    /* EP→RC: bit i => slot i POSTED (credit) */
	__u32 done_mask;      /* EP→RC: bit i => slot i DONE */
	__u32 ep_flags;       /* capability bits, see INFER_EP_F_* */
	__u32 reserved_v2[4];
} __attribute__((packed));

#define INFER_EP_F_ZEROCOPY         (1u << 0)  /* POST_RECV + PUSH supported */
#define INFER_EP_F_DMABUF           (1u << 1)  /* EP accepts dma-buf fd */
#define INFER_EP_F_MULTI_SLOT       (1u << 2)  /* INFER_V2_SLOTS > 1 */

/* =====================================================================
 * RC ioctls — sender on this cable
 * ===================================================================== */

struct infer_push {
	__u32 slot;
	__u32 size;
	__u64 pci_addr;       /* RC-visible bus addr for the peer EP to read */
	__u64 timeout_us;     /* in: timeout; out: latency ns */
	__s32 result;
	__u32 flags;          /* reserved, 0 */
};

#define INFER_IOC_PUSH              _IOWR(INFER_IOC_MAGIC, 10, struct infer_push)

struct infer_map_req {
	__u64 user_ptr;       /* userspace VA (may be unaligned) */
	__u64 size;
	__u64 pci_addr;       /* out: bus addr for PUSH */
	__u32 flags;          /* 0 */
	__u32 reserved;
};

#define INFER_IOC_MAP_USER          _IOWR(INFER_IOC_MAGIC, 11, struct infer_map_req)
#define INFER_IOC_UNMAP_USER        _IOW(INFER_IOC_MAGIC, 12, __u64) /* pci_addr from MAP_USER */

struct infer_credit {
	__u32 posted_mask;
	__u32 done_mask;
	__u32 seq;
	__u32 ep_flags;
};

#define INFER_IOC_GET_CREDIT        _IOR(INFER_IOC_MAGIC, 13, struct infer_credit)

/* Map an NPU/exporter dma-buf into a PCI bus address for PUSH (RC side). */
struct infer_map_dmabuf {
	__s32 dmabuf_fd;
	__u32 dmabuf_offset;
	__u64 size;           /* in: bytes to map from offset; 0 = rest of buf */
	__u64 pci_addr;       /* out: bus addr for PUSH */
	__u32 flags;          /* 0 */
	__u32 reserved;
};

#define INFER_IOC_MAP_DMABUF        _IOWR(INFER_IOC_MAGIC, 14, struct infer_map_dmabuf)
#define INFER_IOC_UNMAP_DMABUF      _IOW(INFER_IOC_MAGIC, 15, __u64) /* pci_addr */

/* =====================================================================
 * EP ioctls — /dev/pci_epf_infer0
 * ===================================================================== */

#define INFER_EP_IOC_MAGIC          'E'

struct infer_ep_recv_reg {
	__u32 slot;
	__u32 flags;          /* INFER_EP_REG_F_* */
	__u64 capacity;       /* in: max accept; for DMABUF may clamp to buf size */
	__u64 local_dst;      /* phys/IOVA when INFER_EP_REG_F_ADDR */
	__s32 dmabuf_fd;      /* when INFER_EP_REG_F_DMABUF */
	__u32 dmabuf_offset;  /* byte offset within dma-buf */
};

#define INFER_EP_REG_F_ADDR         (1u << 0)
#define INFER_EP_REG_F_DMABUF       (1u << 1)
#define INFER_EP_REG_F_STAGING      (1u << 2)

/*
 * POST_RECV = provide this packet's local_dst and mark slot POSTED.
 * Named to avoid confusion with the ARM CPU architecture.
 */
#define INFER_EP_IOC_POST_RECV      _IOW(INFER_EP_IOC_MAGIC, 1, struct infer_ep_recv_reg)
#define INFER_EP_IOC_REG_RECV       INFER_EP_IOC_POST_RECV
/* Deprecated alias (old name); same ioctl number. */
#define INFER_EP_IOC_ARM            INFER_EP_IOC_POST_RECV
#define INFER_EP_IOC_UNREG          _IOW(INFER_EP_IOC_MAGIC, 2, __u32)

struct infer_ep_post {
	__u32 slot;
	__u32 flags;
};

#define INFER_EP_IOC_POST           _IOW(INFER_EP_IOC_MAGIC, 3, struct infer_ep_post)

struct infer_ep_wait {
	__u32 slot;
	__u32 flags;
	__u64 timeout_us;
	__u32 size;
	__u32 seq;
	__s32 result;
};

#define INFER_EP_IOC_WAIT           _IOWR(INFER_EP_IOC_MAGIC, 4, struct infer_ep_wait)

/* Staging buffer info for mmap smoke tests (non-zero-copy). */
struct infer_ep_info {
	__u64 staging_size;
	__u64 staging_dma;    /* local DMA addr; for debug only */
	__u32 ep_flags;
	__u32 n_slots;
};

#define INFER_EP_IOC_GET_INFO       _IOR(INFER_EP_IOC_MAGIC, 5, struct infer_ep_info)

/*
 * EP-side eDMA timing stats (submit → completion callback), accumulated in
 * the driver with zero per-transfer printk. Reading with INFER_EP_IOC_DMA_STATS
 * returns the current counters and, if reset != 0, clears them afterwards.
 *
 * Measures submit→callback: eDMA queue + transfer + completion-IRQ→callback
 * dispatch. It is a subset of the RC end-to-end latency (infer_poll_status),
 * so RC_end_to_end - dma_avg ≈ doorbell IRQ + status write/poll overhead.
 */
struct infer_ep_dma_stats {
	__u32 reset;          /* in: nonzero => zero counters after reading */
	__u32 pad;
	__u64 count;          /* completed DMAs since last reset */
	__u64 sum_ns;         /* sum of submit→cb durations */
	__u64 min_ns;
	__u64 max_ns;
	__u64 last_ns;
	__u64 bytes;          /* total bytes moved */
};

#define INFER_EP_IOC_DMA_STATS      _IOWR(INFER_EP_IOC_MAGIC, 6, struct infer_ep_dma_stats)

#endif /* _INFER_PROTO_V2_H_ */
