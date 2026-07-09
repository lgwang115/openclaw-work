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
cp -v "$SRC/pci_epf_infer.c" "$SRC/infer_proto.h" "$EP_DIR/"

# Ensure include matches in-tree pci-epf-test style
if ! grep -q 'pcie-bst.h' "$EP_DIR/pci_epf_infer.c"; then
	echo "ERROR: pci_epf_infer.c should include pcie-bst.h"
	exit 1
fi

# Add to Makefile once
if ! grep -q 'pci_epf_infer.o' "$EP_DIR/Makefile"; then
	echo 'obj-m += pci_epf_infer.o' >> "$EP_DIR/Makefile"
	echo "appended obj-m += pci_epf_infer.o to $EP_DIR/Makefile"
fi

echo "==> Install RC sources into $MISC_DIR"
cp -v "$SRC/infer_rc.c" "$SRC/infer_proto.h" "$MISC_DIR/"
if ! grep -q 'infer_rc.o' "$MISC_DIR/Makefile"; then
	echo 'obj-m += infer_rc.o' >> "$MISC_DIR/Makefile"
	echo "appended obj-m += infer_rc.o to $MISC_DIR/Makefile"
fi

echo "==> modules_prepare (if needed)"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" modules_prepare

echo "==> Build EP module"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	M=drivers/pci/endpoint/functions pci_epf_infer.ko

echo "==> Build RC module"
make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	M=drivers/misc infer_rc.ko

echo "==> Build userspace tool"
"${CROSS_COMPILE}gcc" -O2 -Wall -I"$SRC" -o "$SRC/inferlat" "$SRC/inferlat.c"

echo
echo "DONE. Artifacts:"
ls -l "$EP_DIR/pci_epf_infer.ko" "$MISC_DIR/infer_rc.ko" "$SRC/inferlat"
echo
echo "Copy to boards:"
echo "  scp $EP_DIR/pci_epf_infer.ko root@<EP>:/userdata/ep_test/"
echo "  scp $MISC_DIR/infer_rc.ko $SRC/inferlat root@<RC>:/userdata/ep_test/"
