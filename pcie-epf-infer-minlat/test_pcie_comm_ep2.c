/*
 * test_pcie_comm_ep2.c — bidirectional v2 PUSH/POST_RECV smoke (no MNN)
 *
 * Mirrors BstEpCommPcie usage for EP-2 bring-up before MoE.
 *
 * Build:
 *   ${CROSS_COMPILE}gcc -O2 -Wall -o test_pcie_comm_ep2 test_pcie_comm_ep2.c
 *
 * Both boards (after EP reprogram + infer_rc):
 *   rank0 (B): ./test_pcie_comm_ep2 0 /dev/infer_rc0
 *   rank1 (A): ./test_pcie_comm_ep2 1 /dev/infer_rc0
 *
 * Critical ordering for all-to-all (also what MoE must do):
 *   post_recv → send → wait_recv
 * Never block on send() on both ranks before either has POST_RECV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "infer_proto_v2.h"

#define SLOT           0
#define PAYLOAD        (50 * 1024)
#define ITERS          100
#define CREDIT_POLL_US 100
#define CREDIT_MAX     50000  /* 5s */

struct peer {
	int rc_fd;
	void *rc_map;
	uint64_t rc_size;
	uint64_t rc_dma;
	int ep_fd;
	void *ep_map;
	uint64_t ep_size;
};

static uint64_t ns_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int wait_credit(int rc_fd)
{
	struct infer_credit cr;
	int n = 0;

	do {
		if (ioctl(rc_fd, INFER_IOC_GET_CREDIT, &cr) == 0 &&
		    (cr.posted_mask & (1u << SLOT)))
			return 0;
		usleep(CREDIT_POLL_US);
	} while (++n < CREDIT_MAX);

	fprintf(stderr, "credit: slot %d not POSTED after 5s (posted=0x%x)\n",
		SLOT, cr.posted_mask);
	return -1;
}

static int post_recv(struct peer *p, size_t capacity)
{
	struct infer_ep_recv_reg post = {
		.slot = SLOT,
		.flags = INFER_EP_REG_F_STAGING,
		.capacity = capacity < p->ep_size ? capacity : p->ep_size,
	};

	if (ioctl(p->ep_fd, INFER_EP_IOC_POST_RECV, &post) < 0) {
		perror("POST_RECV");
		return -1;
	}
	return 0;
}

static int wait_recv(struct peer *p, void *buf, size_t size)
{
	struct infer_ep_wait w = {
		.slot = SLOT,
		.timeout_us = 10000000,
	};
	size_t actual;

	if (ioctl(p->ep_fd, INFER_EP_IOC_WAIT, &w) < 0 || w.result != 0) {
		fprintf(stderr, "WAIT failed ret/result=%d/%d\n", errno, w.result);
		return -1;
	}
	actual = w.size ? w.size : size;
	if (actual > size)
		actual = size;
	memcpy(buf, p->ep_map, actual);
	return 0;
}

static int do_send(struct peer *p, const void *data, size_t size)
{
	struct infer_push push = {
		.slot = SLOT,
		.size = (uint32_t)size,
		.pci_addr = p->rc_dma,
		.timeout_us = 5000000,
	};

	if (size > p->rc_size) {
		fprintf(stderr, "send size %zu > staging %llu\n", size,
			(unsigned long long)p->rc_size);
		return -1;
	}
	memcpy(p->rc_map, data, size);

	if (wait_credit(p->rc_fd) < 0)
		return -1;

	if (ioctl(p->rc_fd, INFER_IOC_PUSH, &push) < 0 || push.result != 0) {
		fprintf(stderr, "PUSH failed result=%d\n", push.result);
		return -1;
	}
	return 0;
}

/*
 * All-to-all safe exchange on one peer (the only pattern MoE should use):
 * both ranks call this with the same steps — post first, then send, then wait.
 */
static int exchange(struct peer *p, const void *send_buf, void *recv_buf,
		    size_t size)
{
	if (post_recv(p, size) < 0)
		return -1;
	/* tiny yield so peer can also POST_RECV before we credit-poll */
	usleep(1000);
	if (do_send(p, send_buf, size) < 0)
		return -1;
	if (wait_recv(p, recv_buf, size) < 0)
		return -1;
	return 0;
}

static int open_peer(struct peer *p, const char *rc_dev)
{
	struct infer_buf_req br = {};
	struct infer_ep_info info = {};

	memset(p, 0, sizeof(*p));
	p->rc_fd = open(rc_dev, O_RDWR);
	if (p->rc_fd < 0) {
		perror(rc_dev);
		return -1;
	}
	if (ioctl(p->rc_fd, INFER_IOC_ALLOC, &br) < 0) {
		perror("ALLOC");
		return -1;
	}
	p->rc_size = br.size;
	p->rc_dma = br.dma_addr;
	p->rc_map = mmap(NULL, br.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 p->rc_fd, 0);
	if (p->rc_map == MAP_FAILED) {
		perror("mmap RC");
		return -1;
	}

	p->ep_fd = open("/dev/pci_epf_infer0", O_RDWR);
	if (p->ep_fd < 0) {
		perror("/dev/pci_epf_infer0");
		return -1;
	}
	if (ioctl(p->ep_fd, INFER_EP_IOC_GET_INFO, &info) < 0) {
		perror("GET_INFO");
		return -1;
	}
	p->ep_size = info.staging_size;
	p->ep_map = mmap(NULL, info.staging_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, p->ep_fd, 0);
	if (p->ep_map == MAP_FAILED) {
		perror("mmap EP");
		return -1;
	}

	printf("RC %s staging=%llu KB dma=0x%llx | EP staging=%llu KB flags=0x%x\n",
	       rc_dev, (unsigned long long)(br.size / 1024),
	       (unsigned long long)br.dma_addr,
	       (unsigned long long)(info.staging_size / 1024), info.ep_flags);
	return 0;
}

static void close_peer(struct peer *p)
{
	if (p->rc_map && p->rc_map != MAP_FAILED)
		munmap(p->rc_map, p->rc_size);
	if (p->ep_map && p->ep_map != MAP_FAILED)
		munmap(p->ep_map, p->ep_size);
	if (p->rc_fd >= 0)
		close(p->rc_fd);
	if (p->ep_fd >= 0)
		close(p->ep_fd);
}

int main(int argc, char **argv)
{
	int rank;
	struct peer p;
	char msg_send[32], msg_recv[32];
	uint8_t *send_buf, *recv_buf;
	uint64_t t_ex = 0;
	int i, ok = 1;

	if (argc < 3) {
		fprintf(stderr,
			"Usage: %s <rank> </dev/infer_rc0>\n"
			"  rank0: %s 0 /dev/infer_rc0\n"
			"  rank1: %s 1 /dev/infer_rc0\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}
	rank = atoi(argv[1]);
	if (open_peer(&p, argv[2]) < 0)
		return 1;

	/* ---- test1: small message via post/send/wait (both ranks same order) ---- */
	snprintf(msg_send, sizeof(msg_send), "hello from rank%d", rank);
	memset(msg_recv, 0, sizeof(msg_recv));
	printf("[rank %d] exchange hello (post→send→wait)...\n", rank);
	if (exchange(&p, msg_send, msg_recv, sizeof(msg_recv)) < 0) {
		fprintf(stderr, "[rank %d] hello exchange FAILED\n", rank);
		close_peer(&p);
		return 1;
	}
	printf("[rank %d] got \"%s\" → %s\n", rank, msg_recv,
	       strstr(msg_recv, "rank") ? "OK" : "FAIL");

	/* ---- test2: 50KB × ITERS ---- */
	send_buf = malloc(PAYLOAD);
	recv_buf = calloc(1, PAYLOAD);
	if (!send_buf || !recv_buf) {
		perror("malloc");
		return 1;
	}
	for (i = 0; i < PAYLOAD; i++)
		send_buf[i] = (uint8_t)(i & 0xff);

	printf("[rank %d] 50KB exchange × %d...\n", rank, ITERS);
	for (i = 0; i < ITERS; i++) {
		uint64_t t0 = ns_now();

		if (exchange(&p, send_buf, recv_buf, PAYLOAD) < 0) {
			fprintf(stderr, "[rank %d] exchange[%d] FAILED\n", rank, i);
			ok = 0;
			break;
		}
		t_ex += ns_now() - t0;
	}

	if (ok) {
		double us = (double)t_ex / ITERS / 1000.0;

		printf("[rank %d] exchange 50KB × %d: avg %.2f µs (%.2f GB/s RTT/2~)\n",
		       rank, ITERS, us, (double)PAYLOAD / (us / 2.0) / 1024.0);
		for (i = 0; i < PAYLOAD; i++) {
			if (recv_buf[i] != (uint8_t)(i & 0xff)) {
				ok = 0;
				break;
			}
		}
		printf("[rank %d] payload integrity: %s\n", rank, ok ? "OK" : "FAIL");
	}

	free(send_buf);
	free(recv_buf);
	close_peer(&p);
	return ok ? 0 : 1;
}
