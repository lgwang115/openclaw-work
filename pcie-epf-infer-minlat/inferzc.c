/*
 * inferzc.c — zero-copy ioctl tests
 *
 * Modes:
 *   ep          EP: dmabuf POST_RECV + verify
 *   rc          RC: MAP_USER + PUSH  (staging VA)
 *   rc-dmabuf   RC: MAP_DMABUF + PUSH (needs infer_dmabuf_test.ko on RC too)
 *
 * EP board (start first):
 *   insmod infer_dmabuf_test.ko
 *   ./inferzc ep [slot] [size] [iters]
 *
 * RC board:
 *   ./inferzc rc [slot] [size]
 *   # or full dmabuf↔dmabuf:
 *   insmod infer_dmabuf_test.ko
 *   ./inferzc rc-dmabuf [slot] [size] [iters]
 *
 * iters (ep / rc-dmabuf, default 1): repeat POST_RECV/PUSH N times so the
 * EP-side eDMA stats (inferdmastat) accumulate real min/avg/max. Run the same
 * iters on both boards. Example, 4K dma-buf timing:
 *   EP: ./inferdmastat reset ; ./inferzc ep 0 4096 1000
 *   RC: ./inferzc rc-dmabuf 0 4096 1000
 *   EP: ./inferdmastat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "infer_proto_v2.h"
#include "infer_dmabuf_test.h"

#define PATTERN_SEED 0x5a

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

static int do_ep(int slot, uint32_t size, int iters)
{
	struct infer_dmabuf_export exp = { .size = size };
	struct infer_ep_recv_reg post = {
		.slot = slot,
		.flags = INFER_EP_REG_F_DMABUF,
		.capacity = size,
		.dmabuf_fd = -1,
	};
	struct infer_ep_info info = {};
	int dfd = -1, efd = -1, ret = 1, i;
	int bad_iters = 0;
	void *map = MAP_FAILED;
	size_t map_size = 0;

	dfd = open("/dev/infer_dmabuf_test", O_RDWR);
	if (dfd < 0) {
		perror("open /dev/infer_dmabuf_test");
		return 1;
	}
	if (ioctl(dfd, INFER_DMABUF_IOC_EXPORT, &exp) < 0) {
		perror("DMABUF EXPORT");
		goto out;
	}
	printf("EP exported dmabuf fd=%d size=%llu iters=%d\n",
	       exp.fd, (unsigned long long)exp.size, iters);
	map_size = exp.size;
	post.dmabuf_fd = exp.fd;

	efd = open("/dev/pci_epf_infer0", O_RDWR);
	if (efd < 0) {
		perror("open /dev/pci_epf_infer0");
		goto out;
	}
	if (ioctl(efd, INFER_EP_IOC_GET_INFO, &info) == 0)
		printf("EP ep_flags=0x%x\n", info.ep_flags);

	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, exp.fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap dmabuf");
		goto out;
	}

	printf("EP POST_RECV DMABUF slot=%d — waiting for %d PUSH (first up to 60s)...\n",
	       slot, iters);

	for (i = 0; i < iters; i++) {
		struct infer_ep_wait w = {
			.slot = slot,
			/* first iter waits long for the RC to start; rest short */
			.timeout_us = (i == 0) ? 60000000 : 5000000,
		};

		memset(map, 0, size);
		if (ioctl(efd, INFER_EP_IOC_POST_RECV, &post) < 0) {
			perror("POST_RECV(DMABUF)");
			goto out;
		}
		if (ioctl(efd, INFER_EP_IOC_WAIT, &w) != 0) {
			fprintf(stderr, "WAIT[%d] failed result=%d\n", i, w.result);
			goto out;
		}
		if (check_pattern(map, w.size ? w.size : size) != 0)
			bad_iters++;
	}

	if (bad_iters == 0) {
		printf("payload OK via EP DMABUF x%d\n", iters);
		ret = 0;
	} else {
		fprintf(stderr, "payload MISMATCH in %d/%d iters\n", bad_iters, iters);
		ret = 1;
	}
out:
	if (map != MAP_FAILED)
		munmap(map, map_size);
	if (exp.fd >= 0)
		close(exp.fd);
	if (efd >= 0)
		close(efd);
	if (dfd >= 0)
		close(dfd);
	return ret;
}

static int rc_push(int fd, int slot, uint32_t size, uint64_t pci_addr)
{
	struct infer_push p = {
		.slot = slot,
		.size = size,
		.pci_addr = pci_addr,
		.timeout_us = 1000000,
	};
	int ret = ioctl(fd, INFER_IOC_PUSH, &p);

	printf("PUSH result=%d latency_ns=%llu\n",
	       ret, (unsigned long long)p.timeout_us);
	return ret ? 1 : 0;
}

/* Wait until EP marks the slot POSTED (credit), then PUSH once. Quiet. */
static int rc_push_when_posted(int fd, int slot, uint32_t size, uint64_t pci_addr)
{
	struct infer_credit cr;
	const uint32_t slot_bit = (1u << slot);
	int n = 0;

	do {
		if (ioctl(fd, INFER_IOC_GET_CREDIT, &cr) == 0 &&
		    (cr.posted_mask & slot_bit))
			break;
		usleep(100);
	} while (++n < 50000);  /* 5s */
	if (n >= 50000) {
		fprintf(stderr, "slot %d not POSTED after 5s (posted=0x%x)\n",
			slot, cr.posted_mask);
		return 1;
	}

	struct infer_push p = {
		.slot = slot,
		.size = size,
		.pci_addr = pci_addr,
		.timeout_us = 5000000,
	};
	if (ioctl(fd, INFER_IOC_PUSH, &p) != 0 || p.result != 0) {
		fprintf(stderr, "PUSH result=%d\n", p.result);
		return 1;
	}
	return 0;
}

static int do_rc_user(int slot, uint32_t size)
{
	struct infer_credit cr;
	struct infer_map_req mr = {};
	int fd, ret = 1;
	void *map = MAP_FAILED;
	size_t map_size;
	__u64 pci = 0;

	/*
	 * MAP_USER needs normal page-backed VA (anonymous/file pages).
	 * Do NOT mmap /dev/infer_rc0 staging: dma_mmap_coherent is VM_PFNMAP
	 * and pin_user_pages returns -EFAULT ("Bad address").
	 * Keep size to one page unless hugepage/contig allocator is used —
	 * multi-page anonymous memory is rarely physically contiguous.
	 */
	map_size = (size + 4095u) & ~4095u;
	if (map_size > 4096) {
		fprintf(stderr,
			"note: MAP_USER test uses anonymous pages; size>4K may fail "
			"contiguity check — prefer ./inferzc rc-dmabuf\n");
	}

	fd = open("/dev/infer_rc0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/infer_rc0");
		return 1;
	}
	if (ioctl(fd, INFER_IOC_GET_CREDIT, &cr) == 0)
		printf("credit posted=0x%x ep_flags=0x%x\n",
		       cr.posted_mask, cr.ep_flags);

	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		perror("mmap anonymous");
		goto out;
	}
	fill_pattern(map, size);

	mr.user_ptr = (uintptr_t)map;
	mr.size = size;
	if (ioctl(fd, INFER_IOC_MAP_USER, &mr) < 0) {
		perror("MAP_USER");
		fprintf(stderr,
			"hint: need page-backed contiguous VA; try ./inferzc rc-dmabuf\n");
		goto out;
	}
	pci = mr.pci_addr;
	printf("MAP_USER -> pci_addr=0x%llx\n", (unsigned long long)pci);

	ret = rc_push(fd, slot, size, pci);
	if (ioctl(fd, INFER_IOC_UNMAP_USER, &pci) < 0)
		perror("UNMAP_USER");
out:
	if (map != MAP_FAILED)
		munmap(map, map_size);
	close(fd);
	return ret;
}

static int do_rc_dmabuf(int slot, uint32_t size, int iters)
{
	struct infer_dmabuf_export exp = { .size = size };
	struct infer_map_dmabuf md = { .dmabuf_fd = -1, .size = size };
	struct infer_credit cr;
	int dfd = -1, fd = -1, ret = 1, i;
	void *map = MAP_FAILED;
	__u64 pci = 0;

	dfd = open("/dev/infer_dmabuf_test", O_RDWR);
	if (dfd < 0) {
		perror("open /dev/infer_dmabuf_test on RC");
		return 1;
	}
	if (ioctl(dfd, INFER_DMABUF_IOC_EXPORT, &exp) < 0) {
		perror("DMABUF EXPORT");
		goto out;
	}
	printf("RC exported dmabuf fd=%d size=%llu iters=%d\n",
	       exp.fd, (unsigned long long)exp.size, iters);

	map = mmap(NULL, exp.size, PROT_READ | PROT_WRITE, MAP_SHARED, exp.fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap dmabuf");
		goto out;
	}
	fill_pattern(map, size);

	fd = open("/dev/infer_rc0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/infer_rc0");
		goto out;
	}
	if (ioctl(fd, INFER_IOC_GET_CREDIT, &cr) == 0)
		printf("credit posted=0x%x\n", cr.posted_mask);

	md.dmabuf_fd = exp.fd;
	md.dmabuf_offset = 0;
	md.size = size;
	if (ioctl(fd, INFER_IOC_MAP_DMABUF, &md) < 0) {
		perror("MAP_DMABUF");
		goto out;
	}
	pci = md.pci_addr;
	printf("MAP_DMABUF -> pci_addr=0x%llx size=%llu\n",
	       (unsigned long long)pci, (unsigned long long)md.size);

	ret = 0;
	for (i = 0; i < iters; i++) {
		if (rc_push_when_posted(fd, slot, size, pci)) {
			fprintf(stderr, "PUSH[%d] failed\n", i);
			ret = 1;
			break;
		}
	}
	if (ret == 0)
		printf("PUSH x%d OK — read EP-side timing with ./inferdmastat\n",
		       iters);

	if (ioctl(fd, INFER_IOC_UNMAP_DMABUF, &pci) < 0)
		perror("UNMAP_DMABUF");
out:
	if (map != MAP_FAILED)
		munmap(map, exp.size);
	if (exp.fd >= 0)
		close(exp.fd);
	if (fd >= 0)
		close(fd);
	if (dfd >= 0)
		close(dfd);
	return ret;
}

int main(int argc, char **argv)
{
	const char *mode;
	int slot = 0;
	uint32_t size = 4096;
	int iters = 1;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s ep|rc|rc-dmabuf [slot] [size] [iters]\n"
			"  iters (ep/rc-dmabuf, default 1): repeat for eDMA stats\n",
			argv[0]);
		return 1;
	}
	mode = argv[1];
	if (argc >= 3)
		slot = atoi(argv[2]);
	if (argc >= 4)
		size = (uint32_t)atoi(argv[3]);
	if (argc >= 5)
		iters = atoi(argv[4]);
	if (iters < 1)
		iters = 1;

	if (!strcmp(mode, "ep"))
		return do_ep(slot, size, iters);
	if (!strcmp(mode, "rc"))
		return do_rc_user(slot, size);
	if (!strcmp(mode, "rc-dmabuf"))
		return do_rc_dmabuf(slot, size, iters);
	fprintf(stderr, "mode must be ep|rc|rc-dmabuf\n");
	return 1;
}
