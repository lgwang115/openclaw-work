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
	/*
	 * hw_done: written by the EP eDMA itself (a 2nd linked-list element)
	 * right after the data lands, so RC sees completion without waiting for
	 * the completion IRQ/callback. RC resets it to 0 before ringing and
	 * polls for INFER_HW_DONE_MAGIC. status (callback-written) stays as a
	 * fallback. Only for DEV_TO_MEM (EP reads RC → local) with a trailer.
	 */
	__u32 hw_done;
	/*
	 * xfer_flags: RC → EP per-command. INFER_XF_HWDONE means RC placed the
	 * completion tag at pci_addr+size and wants the EP eDMA to append a 2nd
	 * LL element that writes it into hw_done (fast completion, no IRQ wait).
	 */
	__u32 xfer_flags;
	__u32 reserved_v2[2];
} __attribute__((packed));

#define INFER_HW_DONE_MAGIC         0x00d02e00u  /* eDMA-written completion tag */
#define INFER_XF_HWDONE             (1u << 0)    /* append eDMA hw_done element */

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

/* =====================================================================
 * HDMA register debug (RC side) — poke the EP's DesignWare HDMA regs via
 * BAR0 + BST_TRGT0_HDMA_BASE. For bring-up/verification of the direct-HDMA
 * completion path; NOT used by the data path. offset is relative to the
 * HDMA register block base (i.e. BAR0 + 0x4000).
 * ===================================================================== */

/* BST target0 layout (BAR0): doorbell @ 0xE00, HDMA regs @ 0x4000.
 * Named INFER_* to avoid clashing with BST_TRGT0_HDMA_BASE in pcie-bst.h,
 * which pci_epf_infer.c also includes. */
#define INFER_HDMA_BASE             0x4000u

/* dw-hdma-v0 per-channel block */
#define HDMA_V0_CH_STRIDE           0x200u   /* channel i base = i*0x200 */
#define HDMA_V0_RD_OFF              0x100u   /* read block within a channel */
#define HDMA_V0_WR_OFF              0x000u   /* write block within a channel */
#define HDMA_V0_CH_EN               0x00u
#define HDMA_V0_CH_DOORBELL         0x04u
#define HDMA_V0_CH_XFERSIZE         0x1cu
#define HDMA_V0_CH_SAR_LO           0x20u
#define HDMA_V0_CH_SAR_HI           0x24u
#define HDMA_V0_CH_DAR_LO           0x28u
#define HDMA_V0_CH_DAR_HI           0x2cu
#define HDMA_V0_CH_STAT             0x80u   /* [2:0] 1=RUN 2=ABORT 3=STOP */
#define HDMA_V0_CH_INT_STAT         0x84u   /* bit0=STOP bit2=ABORT */
#define HDMA_V0_CH_INT_SETUP        0x88u
#define HDMA_V0_CH_INT_CLEAR        0x8cu

#define HDMA_V0_STAT_MASK           0x7u
#define HDMA_V0_STAT_RUNNING        0x1u
#define HDMA_V0_STAT_ABORTED        0x2u
#define HDMA_V0_STAT_STOPPED        0x3u
#define HDMA_V0_STOP_INT            (1u << 0)
#define HDMA_V0_ABORT_INT           (1u << 2)

/* rd-channel register offset from HDMA base (0x4000) */
#define HDMA_V0_RD_REG(ch, reg) \
	((ch) * HDMA_V0_CH_STRIDE + HDMA_V0_RD_OFF + (reg))
#define HDMA_V0_WR_REG(ch, reg) \
	((ch) * HDMA_V0_CH_STRIDE + HDMA_V0_WR_OFF + (reg))

struct infer_hdma_dbg {
	__u32 offset;    /* byte offset from HDMA base (BAR0+0x4000), 4-aligned */
	__u32 is_write;  /* nonzero: writel(value) before reading back */
	__u32 value;     /* in: value to write */
	__u32 out;       /* out: value read back */
};

#define INFER_IOC_HDMA_DBG          _IOWR(INFER_IOC_MAGIC, 16, struct infer_hdma_dbg)

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
	/* eDMA issue → completion callback (transfer + completion dispatch) */
	__u64 sum_ns;
	__u64 min_ns;
	__u64 max_ns;
	__u64 last_ns;
	__u64 bytes;          /* total bytes moved */
	/* per-segment breakdown (same count), all EP-local clock */
	__u64 prologue_sum_ns; /* handler entry → dma_submit entry (parse+lock) */
	__u64 prologue_min_ns;
	__u64 prologue_max_ns;
	__u64 setup_sum_ns;    /* slave_config + prep_slave_single + submit */
	__u64 setup_min_ns;
	__u64 setup_max_ns;
	__u64 total_sum_ns;    /* handler entry → completion callback (EP internal) */
	__u64 total_min_ns;
	__u64 total_max_ns;
};

#define INFER_EP_IOC_DMA_STATS      _IOWR(INFER_EP_IOC_MAGIC, 6, struct infer_ep_dma_stats)

#endif /* _INFER_PROTO_V2_H_ */
