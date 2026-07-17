/*
 * inferhdma.c — RC-side debug peek/poke of the EP's DesignWare HDMA regs
 *               via /dev/infer_rc0 (BAR0 + 0x4000). Bring-up/verification
 *               only; not part of the data path.
 *
 * Build: ${CROSS_COMPILE}gcc -O2 -Wall -o inferhdma inferhdma.c
 *
 * Usage:
 *   ./inferhdma peek <hex_off>            # read reg at HDMA_base+off
 *   ./inferhdma poke <hex_off> <hex_val>  # write then read back
 *   ./inferhdma rdch <ch>                 # dump read-channel ch regs
 *   ./inferhdma wrch <ch>                 # dump write-channel ch regs
 *   ./inferhdma clr  <ch>                 # clear read-channel ch int_stat
 *
 * Offsets are relative to the HDMA block base (BAR0 + 0x4000).
 * PUSH uses read channel 0; a free dedicated channel is read channel 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "infer_proto_v2.h"

static int g_fd = -1;

static int hdma_rw(uint32_t off, int is_write, uint32_t val, uint32_t *out)
{
	struct infer_hdma_dbg d = { .offset = off, .is_write = is_write, .value = val };

	if (ioctl(g_fd, INFER_IOC_HDMA_DBG, &d) < 0) {
		perror("INFER_IOC_HDMA_DBG");
		return -1;
	}
	if (out)
		*out = d.out;
	return 0;
}

static uint32_t rd(uint32_t off)
{
	uint32_t v = 0;

	hdma_rw(off, 0, 0, &v);
	return v;
}

static const char *stat_str(uint32_t s)
{
	switch (s & HDMA_V0_STAT_MASK) {
	case HDMA_V0_STAT_RUNNING: return "RUNNING";
	case HDMA_V0_STAT_ABORTED: return "ABORTED";
	case HDMA_V0_STAT_STOPPED: return "STOPPED";
	default: return "reserved/idle";
	}
}

static void dump_ch(int ch, int is_rd)
{
	uint32_t base = is_rd ? HDMA_V0_RD_REG(ch, 0) : HDMA_V0_WR_REG(ch, 0);
	uint32_t chs = rd(base + HDMA_V0_CH_STAT);
	uint32_t is  = rd(base + HDMA_V0_CH_INT_STAT);

	printf("%s ch%d (HDMA+0x%x):\n", is_rd ? "RD" : "WR", ch, base);
	printf("  ch_en      =0x%08x\n", rd(base + HDMA_V0_CH_EN));
	printf("  xfer_size  =0x%08x\n", rd(base + HDMA_V0_CH_XFERSIZE));
	printf("  sar        =0x%08x%08x\n",
	       rd(base + HDMA_V0_CH_SAR_HI), rd(base + HDMA_V0_CH_SAR_LO));
	printf("  dar        =0x%08x%08x\n",
	       rd(base + HDMA_V0_CH_DAR_HI), rd(base + HDMA_V0_CH_DAR_LO));
	printf("  ch_stat    =0x%08x  (%s)\n", chs, stat_str(chs));
	printf("  int_stat   =0x%08x  (stop=%d abort=%d)\n", is,
	       !!(is & HDMA_V0_STOP_INT), !!(is & HDMA_V0_ABORT_INT));
	printf("  int_setup  =0x%08x\n", rd(base + HDMA_V0_CH_INT_SETUP));
}

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s peek <off> | poke <off> <val> | "
			"rdch <ch> | wrch <ch> | clr <ch>\n", argv[0]);
		return 1;
	}
	cmd = argv[1];

	g_fd = open("/dev/infer_rc0", O_RDWR);
	if (g_fd < 0) {
		perror("open /dev/infer_rc0");
		return 1;
	}

	if (!strcmp(cmd, "peek") && argc >= 3) {
		uint32_t off = strtoul(argv[2], NULL, 0), v = 0;

		if (hdma_rw(off, 0, 0, &v))
			return 1;
		printf("HDMA+0x%x = 0x%08x\n", off, v);
	} else if (!strcmp(cmd, "poke") && argc >= 4) {
		uint32_t off = strtoul(argv[2], NULL, 0);
		uint32_t val = strtoul(argv[3], NULL, 0), v = 0;

		if (hdma_rw(off, 1, val, &v))
			return 1;
		printf("HDMA+0x%x <= 0x%08x, readback 0x%08x\n", off, val, v);
	} else if (!strcmp(cmd, "rdch") && argc >= 3) {
		dump_ch(atoi(argv[2]), 1);
	} else if (!strcmp(cmd, "wrch") && argc >= 3) {
		dump_ch(atoi(argv[2]), 0);
	} else if (!strcmp(cmd, "clr") && argc >= 3) {
		int ch = atoi(argv[2]);
		uint32_t off = HDMA_V0_RD_REG(ch, HDMA_V0_CH_INT_CLEAR), v = 0;

		if (hdma_rw(off, 1, HDMA_V0_STOP_INT | HDMA_V0_ABORT_INT, &v))
			return 1;
		printf("RD ch%d int_clear written; int_stat now 0x%08x\n",
		       ch, rd(HDMA_V0_RD_REG(ch, HDMA_V0_CH_INT_STAT)));
	} else {
		fprintf(stderr, "bad args\n");
		return 1;
	}

	close(g_fd);
	return 0;
}
