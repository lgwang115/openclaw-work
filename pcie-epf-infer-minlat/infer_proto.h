/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared protocol between EP (pci_epf_infer) and RC (infer_rc).
 * Keep this header identical in kernel modules and userspace.
 */
#ifndef _INFER_PROTO_H_
#define _INFER_PROTO_H_

#define INFER_MAGIC                 0x494e4652u  /* 'INFR' */

#define INFER_CMD_NONE              0
#define INFER_CMD_WRITE             1  /* RC -> EP: EP DMA-reads RC memory */
#define INFER_CMD_READ              2  /* EP -> RC: EP DMA-writes RC memory */

#define INFER_STATUS_IDLE           0
#define INFER_STATUS_OK             1
#define INFER_STATUS_FAIL           2
#define INFER_STATUS_BUSY           3

#define INFER_VENDOR_ID             0x1ef1
#define INFER_DEVICE_ID             0x0301

#define INFER_BAR0_SIZE             (64 * 1024)
#define INFER_MAX_XFER              (4 * 1024 * 1024)  /* 4MB prealloc slot */

/*
 * BAR0 layout (control registers). All fields little-endian.
 * RC writes command fields then rings doorbell; EP clears command,
 * runs DMA, then writes status. RC polls status (no MSI on critical path).
 */
struct infer_regs {
	__u32 magic;
	__u32 command;
	__u32 status;
	__u32 size;
	__u64 pci_addr;       /* RC-side DMA address visible to EP */
	__u32 db_bar;         /* filled by EP at bind: doorbell location */
	__u32 db_offset;
	__u32 db_msg;
	__u32 seq;            /* optional: RC increments each request */
	__u32 reserved[4];
} __attribute__((packed));

/* RC driver ioctls */
#define INFER_IOC_MAGIC             'I'
#define INFER_IOC_GET_DB            _IOR(INFER_IOC_MAGIC, 1, struct infer_db_info)
#define INFER_IOC_ALLOC             _IOWR(INFER_IOC_MAGIC, 2, struct infer_buf_req)
#define INFER_IOC_FREE              _IO(INFER_IOC_MAGIC, 3)
#define INFER_IOC_XFER              _IOWR(INFER_IOC_MAGIC, 4, struct infer_xfer)

struct infer_db_info {
	__u32 bar;
	__u32 offset;
	__u32 msg;
};

struct infer_buf_req {
	__u64 size;           /* in: requested, out: granted */
	__u64 dma_addr;       /* out: PCI/DMA address for EP */
	__u64 user_ptr;       /* out: userspace mmap offset token (=0 for single buf) */
};

struct infer_xfer {
	__u32 cmd;            /* INFER_CMD_WRITE or INFER_CMD_READ */
	__u32 size;
	__u64 timeout_us;     /* in: poll timeout; out: measured latency ns */
	__s32 result;         /* out: 0 ok, <0 errno-style */
};

#endif /* _INFER_PROTO_H_ */
