#!/bin/bash
# Install sources into linux-6.6 tree and build as in-tree modules.
# Usage:
#   ./install-into-kernel.sh /home/bst/workspace/linux-6.6
set -euo pipefail

KDIR="${1:-$HOME/workspace/linux-6.6}"
SRC="$(cd "$(dirname "$0")" && pwd)"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

if [[ ! -d "$KDIR/drivers/pci/endpoint/functions" ]]; then
	echo "ERROR: not a kernel tree: $KDIR"
	exit 1
fi

REL="$(make -s -C "$KDIR" ARCH="$ARCH" kernelrelease 2>/dev/null || true)"
echo "KDIR=$KDIR"
echo "kernelrelease=$REL"
if [[ "$REL" != "6.6.64-rt47" ]]; then
	echo "WARNING: expected 6.6.64-rt47, got '$REL'"
	echo "Continue anyway? (modules may not load on board) [y/N]"
	read -r ans
	[[ "$ans" == "y" || "$ans" == "Y" ]] || exit 1
fi

EP_DIR="$KDIR/drivers/pci/endpoint/functions"
MISC_DIR="$KDIR/drivers/misc"

echo "==> Install EP sources into $EP_DIR"
cp -v "$SRC/pci_epf_infer.c" "$SRC/infer_proto.h" "$SRC/infer_proto_v2.h" "$EP_DIR/"

if ! grep -q 'pcie-bst.h' "$EP_DIR/pci_epf_infer.c"; then
	echo "ERROR: pci_epf_infer.c should include pcie-bst.h"
	exit 1
fi

if ! grep -q 'pci_epf_infer.o' "$EP_DIR/Makefile"; then
	echo 'obj-m += pci_epf_infer.o' >> "$EP_DIR/Makefile"
	echo "appended obj-m += pci_epf_infer.o to $EP_DIR/Makefile"
fi

echo "==> Install RC + dmabuf test into $MISC_DIR"
cp -v "$SRC/infer_rc.c" "$SRC/infer_proto.h" "$SRC/infer_proto_v2.h" "$MISC_DIR/"
cp -v "$SRC/infer_dmabuf_test.c" "$SRC/infer_dmabuf_test.h" "$MISC_DIR/"
if ! grep -q 'infer_rc.o' "$MISC_DIR/Makefile"; then
	echo 'obj-m += infer_rc.o' >> "$MISC_DIR/Makefile"
	echo "appended obj-m += infer_rc.o to $MISC_DIR/Makefile"
fi
if ! grep -q 'infer_dmabuf_test.o' "$MISC_DIR/Makefile"; then
	echo 'obj-m += infer_dmabuf_test.o' >> "$MISC_DIR/Makefile"
	echo "appended obj-m += infer_dmabuf_test.o to $MISC_DIR/Makefile"
fi

echo "==> modules_prepare (if needed)"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" modules_prepare

echo "==> Build EP module"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	M=drivers/pci/endpoint/functions pci_epf_infer.ko

echo "==> Build RC + dmabuf test modules"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	M=drivers/misc infer_rc.ko infer_dmabuf_test.ko

echo "==> Build userspace tools"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferlat" "$SRC/inferlat.c"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferpush" "$SRC/inferpush.c"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferzc" "$SRC/inferzc.c"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/test_pcie_comm_ep2" "$SRC/test_pcie_comm_ep2.c"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferdmastat" "$SRC/inferdmastat.c"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferhdma" "$SRC/inferhdma.c"

echo
echo "DONE. Artifacts:"
ls -l "$EP_DIR/pci_epf_infer.ko" "$MISC_DIR/infer_rc.ko" \
	"$MISC_DIR/infer_dmabuf_test.ko" \
	"$SRC/inferlat" "$SRC/inferpush" "$SRC/inferzc" \
	"$SRC/inferdmastat" "$SRC/inferhdma" "$SRC/test_pcie_comm_ep2"
echo
echo "Copy to BOTH boards:"
echo "  for IP in <A_IP> <B_IP>; do"
echo "    scp $EP_DIR/pci_epf_infer.ko $MISC_DIR/infer_rc.ko \\"
echo "        $MISC_DIR/infer_dmabuf_test.ko \\"
echo "        $SRC/inferlat $SRC/inferpush $SRC/inferzc \\"
echo "        $SRC/inferdmastat $SRC/inferhdma $SRC/test_pcie_comm_ep2 \\"
echo "        root@\$IP:/userdata/ep_test/"
echo "  done"
echo
echo "Tests:"
echo "  v1 latency:  ./inferlat /dev/infer_rc0 w 100"
echo "  v2 staging:  EP ./inferpush ep 0 4096 ; RC ./inferpush rc 0 4096"
echo "  v2 bidir:    ./test_pcie_comm_ep2 0 /dev/infer_rc0   # other board: rank 1"
echo "  v2 zc path:  EP: insmod infer_dmabuf_test.ko && ./inferzc ep 0 4096"
echo "               RC: ./inferzc rc 0 4096            # MAP_USER"
echo "               RC: ./inferzc rc-dmabuf 0 4096     # MAP_DMABUF (also needs infer_dmabuf_test.ko)"
echo "  EP DMA time: EP ./inferdmastat reset ; (run RC test) ; EP ./inferdmastat"
echo "               or: insmod pci_epf_infer.ko dma_log_every=1000 (dmesg summary)"
echo "See ZEROCOPY.md / BRINGUP.md / EP2_COMM.md."
