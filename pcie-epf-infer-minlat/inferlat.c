/*
 * inferlat.c — userspace latency sweep for pci_epf_infer (4KB .. 2MB)
 *
 * gcc -O2 -o inferlat inferlat.c
 * ./inferlat /dev/infer_rc0 w 100
 * ./inferlat /dev/infer_rc0 r 100
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "infer_proto.h"

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/infer_rc0";
	int wr = 1, iters = 100, fd;
	struct infer_buf_req br;
	struct infer_db_info db;
	uint64_t *lat;
	char sbuf[16];
	void *map;

	if (argc >= 2)
		dev = argv[1];
	if (argc >= 3)
		wr = !strcmp(argv[2], "w");
	if (argc >= 4)
		iters = atoi(argv[3]);

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (ioctl(fd, INFER_IOC_GET_DB, &db) == 0)
		printf("doorbell bar=%u offset=0x%x msg=0x%x\n",
		       db.bar, db.offset, db.msg);

	if (ioctl(fd, INFER_IOC_ALLOC, &br) < 0) {
		perror("ALLOC");
		return 1;
	}
	printf("buffer %llu bytes dma=0x%llx\n",
	       (unsigned long long)br.size, (unsigned long long)br.dma_addr);

	map = mmap(NULL, br.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map != MAP_FAILED)
		memset(map, 0x5a, br.size < 4096 ? br.size : 4096);

	lat = calloc(iters, sizeof(*lat));
	printf("方向: %s  次数/档: %d\n",
	       wr ? "RC->EP (WRITE)" : "EP->RC (READ)", iters);
	printf("%-8s %10s %10s %10s %10s %10s %12s\n",
	       "块大小", "最小(us)", "中位(us)", "平均(us)",
	       "99%(us)", "最大(us)", "带宽(MB/s)");

	for (uint64_t size = 4096; size <= 2ull * 1024 * 1024; size <<= 1) {
		int done = 0;

		if (size > br.size) {
			printf("%-8s  skip (>%llu buffer)\n", ">",
			       (unsigned long long)br.size);
			break;
		}
		for (int i = 0; i < iters; i++) {
			struct infer_xfer x = {
				.cmd = wr ? INFER_CMD_WRITE : INFER_CMD_READ,
				.size = size,
				.timeout_us = 1000000,
			};
			int ret = ioctl(fd, INFER_IOC_XFER, &x);
			if (ret < 0 || x.result) {
				printf("%-8s  FAIL result=%d\n",
				       size >= (1 << 20) ?
				       (sprintf(sbuf, "%lluMB",
						(unsigned long long)(size >> 20)), sbuf) :
				       (sprintf(sbuf, "%lluKB",
						(unsigned long long)(size >> 10)), sbuf),
				       x.result);
				done = 0;
				break;
			}
			lat[done++] = x.timeout_us; /* driver returns ns */
		}
		if (!done)
			continue;

		qsort(lat, done, sizeof(*lat), cmp_u64);
		uint64_t sum = 0;
		for (int i = 0; i < done; i++)
			sum += lat[i];
		double p50_us = lat[done / 2] / 1e3;
		if (size >= (1 << 20))
			sprintf(sbuf, "%lluMB", (unsigned long long)(size >> 20));
		else
			sprintf(sbuf, "%lluKB", (unsigned long long)(size >> 10));
		printf("%-8s %10.1f %10.1f %10.1f %10.1f %10.1f %12.0f\n",
		       sbuf,
		       lat[0] / 1e3, p50_us, sum / (double)done / 1e3,
		       lat[(int)(done * 0.99)] / 1e3, lat[done - 1] / 1e3,
		       size / p50_us * 1e6 / (1024.0 * 1024.0));
	}

	free(lat);
	close(fd);
	return 0;
}
