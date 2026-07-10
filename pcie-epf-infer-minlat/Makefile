# Out-of-tree Makefile — NOT recommended for pci_epf_infer.
# Prefer: ./install-into-kernel.sh /path/to/linux-6.6
#
#   make KDIR=~/workspace/linux-6.6 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m += pci_epf_infer.o
obj-m += infer_rc.o
obj-m += infer_dmabuf_test.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KDIR_ABS := $(abspath $(KDIR))

ccflags-y += -I$(src)

.PHONY: all modules clean inferlat inferpush inferzc test_pcie_comm_ep2 check-version

all: check-version modules inferlat inferpush inferzc test_pcie_comm_ep2

check-version:
	@rel=$$(make -s -C $(KDIR_ABS) ARCH=$(ARCH) kernelrelease); \
	echo "kernelrelease=$$rel"; \
	if [ "$$rel" != "6.6.64-rt47" ]; then \
		echo "WARNING: want 6.6.64-rt47, got $$rel"; \
	fi

modules:
	@echo "NOTE: prefer ./install-into-kernel.sh (EP needs in-tree pcie-bst.h)."
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

inferlat: inferlat.c infer_proto.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o inferlat inferlat.c

inferpush: inferpush.c infer_proto.h infer_proto_v2.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o inferpush inferpush.c

inferzc: inferzc.c infer_proto.h infer_proto_v2.h infer_dmabuf_test.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o inferzc inferzc.c

test_pcie_comm_ep2: test_pcie_comm_ep2.c infer_proto.h infer_proto_v2.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o test_pcie_comm_ep2 test_pcie_comm_ep2.c

clean:
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) clean 2>/dev/null || true
	rm -f inferlat inferpush inferzc test_pcie_comm_ep2
