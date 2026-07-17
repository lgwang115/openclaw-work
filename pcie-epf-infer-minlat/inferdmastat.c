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

	double n = (double)s.count;
	double xfer_avg = s.sum_ns / n / 1000.0;
	double gbps = xfer_avg > 0
		? (double)(s.bytes / s.count) / xfer_avg / 1000.0
		: 0.0;

	printf("EP-internal breakdown  n=%llu  size=%llu B/xfer\n",
	       (unsigned long long)s.count,
	       (unsigned long long)(s.bytes / s.count));
	printf("  %-28s min=%.2f  avg=%.2f  max=%.2f µs\n",
	       "prologue (parse+lock):",
	       s.prologue_min_ns / 1000.0, s.prologue_sum_ns / n / 1000.0,
	       s.prologue_max_ns / 1000.0);
	printf("  %-28s min=%.2f  avg=%.2f  max=%.2f µs\n",
	       "setup (slave_cfg+prep):",
	       s.setup_min_ns / 1000.0, s.setup_sum_ns / n / 1000.0,
	       s.setup_max_ns / 1000.0);
	printf("  %-28s min=%.2f  avg=%.2f  max=%.2f µs  (~%.2f GB/s)\n",
	       "xfer (issue->cb):",
	       s.min_ns / 1000.0, xfer_avg, s.max_ns / 1000.0, gbps);
	printf("  %-28s min=%.2f  avg=%.2f  max=%.2f µs\n",
	       "EP total (handler->cb):",
	       s.total_min_ns / 1000.0, s.total_sum_ns / n / 1000.0,
	       s.total_max_ns / 1000.0);
	printf("Note: doorbell->handler (IRQ latency) is NOT here (cross-chip clock);\n"
	       "      RC end-to-end (inferzc RC total) - EP total = doorbell+IRQ+status+poll.\n"
	       "      For pure IRQ latency use ftrace (irqsoff / irq_handler_entry).\n");
	if (reset)
		printf("  (counters reset)\n");
	return 0;
}
