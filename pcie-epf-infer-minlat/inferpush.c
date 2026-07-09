/*
 * inferpush.c — smoke test for v2 PUSH/ARM path (staging on EP)
 *
 * Build: ${CROSS_COMPILE}gcc -O2 -Wall -o inferpush inferpush.c
 *
 * On EP board (receiver):
 *   ./inferpush ep [slot] [size]
 * On RC board (sender), after EP is armed:
 *   ./inferpush rc [slot] [size]
 *
 * EP uses INFER_EP_REG_F_STAGING (driver 4MB buffer) for this smoke test.
 * Replace with NPU local_dst later via ARM flags ADDR/DMABUF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

#include "infer_proto_v2.h"

static int do_ep(int slot, uint32_t size)
{
	struct infer_ep_recv_reg arm = {
		.slot = slot,
		.flags = INFER_EP_REG_F_STAGING,
		.capacity = size,
	};
	struct infer_ep_wait w = {
		.slot = slot,
		.timeout_us = 5000000,
	};
	int fd, ret;

	fd = open("/dev/pci_epf_infer0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/pci_epf_infer0");
		return 1;
	}

	ret = ioctl(fd, INFER_EP_IOC_ARM, &arm);
	if (ret < 0) {
		perror("ARM");
		return 1;
	}
	printf("EP ARM slot=%d staging size=%u — waiting for PUSH...\n",
	       slot, size);

	ret = ioctl(fd, INFER_EP_IOC_WAIT, &w);
	printf("WAIT result=%d size=%u seq=%u errno_style=%d\n",
	       ret, w.size, w.seq, w.result);
	close(fd);
	return ret ? 1 : 0;
}

static int do_rc(int slot, uint32_t size)
{
	struct infer_buf_req br;
	struct infer_credit cr;
	struct infer_push p = {
		.slot = slot,
		.size = size,
		.timeout_us = 1000000,
	};
	int fd, ret;
	void *map;

	fd = open("/dev/infer_rc0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/infer_rc0");
		return 1;
	}

	if (ioctl(fd, INFER_IOC_GET_CREDIT, &cr) == 0)
		printf("credit posted=0x%x done=0x%x seq=%u ep_flags=0x%x\n",
		       cr.posted_mask, cr.done_mask, cr.seq, cr.ep_flags);

	if (ioctl(fd, INFER_IOC_ALLOC, &br) < 0) {
		perror("ALLOC");
		return 1;
	}
	if (size > br.size) {
		fprintf(stderr, "size %u > staging %llu\n", size,
			(unsigned long long)br.size);
		return 1;
	}

	map = mmap(NULL, br.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map != MAP_FAILED)
		memset(map, 0xa5, size);

	p.pci_addr = br.dma_addr; /* smoke: push from RC staging */
	ret = ioctl(fd, INFER_IOC_PUSH, &p);
	printf("PUSH result=%d latency_ns=%llu\n",
	       ret, (unsigned long long)p.timeout_us);
	if (map != MAP_FAILED)
		munmap(map, br.size);
	close(fd);
	return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *mode;
	int slot = 0;
	uint32_t size = 4096;

	if (argc < 2) {
		fprintf(stderr, "usage: %s ep|rc [slot] [size]\n", argv[0]);
		return 1;
	}
	mode = argv[1];
	if (argc >= 3)
		slot = atoi(argv[2]);
	if (argc >= 4)
		size = (uint32_t)atoi(argv[3]);

	if (!strcmp(mode, "ep"))
		return do_ep(slot, size);
	if (!strcmp(mode, "rc"))
		return do_rc(slot, size);
	fprintf(stderr, "mode must be ep or rc\n");
	return 1;
}
