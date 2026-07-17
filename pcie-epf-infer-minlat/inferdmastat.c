/*
 * inferdmastat.c — read EP-side eDMA timing stats from /dev/pci_epf_infer0
 *
 * These are accumulated in pci_epf_infer with zero per-transfer printk;
 * this tool just reads (and optionally resets) the counters.
 *
 * Build: ${CROSS_COMPILE}gcc -O2 -Wall -o inferdmastat inferdmastat.c
 *
 * Typical use on the EP board:
 *   ./inferdmastat reset          # zero counters
 *   # (run inferlat / inferpush / test from the RC board)
 *   ./inferdmastat                # print accumulated submit→cb timing
 *
 * Compare avg here with the RC end-to-end latency (inferlat) to see how much
 * of the ~17µs is eDMA vs doorbell IRQ + status poll.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "infer_proto_v2.h"

int main(int argc, char **argv)
{
	struct infer_ep_dma_stats s = {};
	int fd, reset = 0;

	if (argc > 1 && strcmp(argv[1], "reset") == 0)
		reset = 1;

	fd = open("/dev/pci_epf_infer0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/pci_epf_infer0");
		return 1;
	}

	s.reset = reset;
	if (ioctl(fd, INFER_EP_IOC_DMA_STATS, &s) < 0) {
		perror("INFER_EP_IOC_DMA_STATS");
		close(fd);
		return 1;
	}
	close(fd);

	if (s.count == 0) {
		printf("eDMA stats: no completed transfers%s\n",
		       reset ? " (counters reset)" : "");
		return 0;
	}

	double avg_us = (double)s.sum_ns / s.count / 1000.0;
	double min_us = s.min_ns / 1000.0;
	double max_us = s.max_ns / 1000.0;
	double last_us = s.last_ns / 1000.0;
	double gbps = avg_us > 0
		? (double)(s.bytes / s.count) / avg_us / 1000.0
		: 0.0;

	printf("eDMA submit->cb: n=%llu  min=%.2fµs  avg=%.2fµs  max=%.2fµs  last=%.2fµs\n",
	       (unsigned long long)s.count, min_us, avg_us, max_us, last_us);
	printf("  total_bytes=%llu  avg_per_xfer=%llu B  ~avg_rate=%.2f GB/s\n",
	       (unsigned long long)s.bytes,
	       (unsigned long long)(s.bytes / s.count), gbps);
	if (reset)
		printf("  (counters reset)\n");
	return 0;
}
