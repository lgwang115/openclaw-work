/*
 * inferpush.c — smoke test for v2 PUSH/POST_RECV (staging, with payload check)
 *
 * Build: ${CROSS_COMPILE}gcc -O2 -Wall -o inferpush inferpush.c
 *
 * Receiver (EP board) — start first, waits up to 60s:
 *   ./inferpush ep [slot] [size]
 * Sender (RC board) — within 60s:
 *   ./inferpush rc [slot] [size]
 *
 * Fills RC staging with a known pattern, PUSH, then EP mmap-verifies bytes.
 * NPU addresses come later via POST_RECV ADDR / PUSH.pci_addr from runtime.
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

#define PATTERN_SEED 0xa5

static void fill_pattern(uint8_t *p, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++)
		p[i] = (uint8_t)(PATTERN_SEED ^ (i & 0xff));
}

static int check_pattern(const uint8_t *p, uint32_t size)
{
	uint32_t bad = 0;

	for (uint32_t i = 0; i < size; i++) {
		uint8_t want = (uint8_t)(PATTERN_SEED ^ (i & 0xff));
		if (p[i] != want) {
			if (bad < 4)
				fprintf(stderr,
					"mismatch @%u got=0x%02x want=0x%02x\n",
					i, p[i], want);
			bad++;
		}
	}
	return bad ? -1 : 0;
}

static int do_ep(int slot, uint32_t size)
{
	struct infer_ep_info info = {};
	struct infer_ep_recv_reg post = {
		.slot = slot,
		.flags = INFER_EP_REG_F_STAGING,
		.capacity = size,
	};
	struct infer_ep_wait w = {
		.slot = slot,
		.timeout_us = 60000000, /* 60s — start RC after EP is waiting */
	};
	int fd, ret;
	void *map = MAP_FAILED;

	fd = open("/dev/pci_epf_infer0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/pci_epf_infer0");
		return 1;
	}

	if (ioctl(fd, INFER_EP_IOC_GET_INFO, &info) == 0)
		printf("EP staging_size=%llu ep_flags=0x%x slots=%u\n",
		       (unsigned long long)info.staging_size, info.ep_flags,
		       info.n_slots);

	if (info.staging_size && size > info.staging_size) {
		fprintf(stderr, "size %u > staging %llu\n", size,
			(unsigned long long)info.staging_size);
		close(fd);
		return 1;
	}

	if (info.staging_size) {
		map = mmap(NULL, info.staging_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			perror("mmap staging (verify skipped)");
		else
			memset(map, 0, size < info.staging_size ? size : 64);
	}

	ret = ioctl(fd, INFER_EP_IOC_POST_RECV, &post);
	if (ret < 0) {
		perror("POST_RECV");
		goto out;
	}
	printf("EP POST_RECV slot=%d staging size=%u — waiting for PUSH (60s)...\n",
	       slot, size);

	ret = ioctl(fd, INFER_EP_IOC_WAIT, &w);
	printf("WAIT result=%d size=%u seq=%u errno_style=%d\n",
	       ret, w.size, w.seq, w.result);
	if (ret)
		goto out;

	if (map != MAP_FAILED) {
		if (check_pattern(map, w.size ? w.size : size) == 0)
			printf("payload OK (pattern 0x%02x^i)\n", PATTERN_SEED);
		else {
			fprintf(stderr, "payload MISMATCH\n");
			ret = -1;
		}
	}

out:
	if (map != MAP_FAILED)
		munmap(map, info.staging_size);
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

	if (!(cr.posted_mask & (1u << slot)))
		fprintf(stderr,
			"warning: slot %d not POSTED yet — start ./inferpush ep first\n",
			slot);

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
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}
	fill_pattern(map, size);

	p.pci_addr = br.dma_addr;
	ret = ioctl(fd, INFER_IOC_PUSH, &p);
	printf("PUSH result=%d latency_ns=%llu\n",
	       ret, (unsigned long long)p.timeout_us);
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
