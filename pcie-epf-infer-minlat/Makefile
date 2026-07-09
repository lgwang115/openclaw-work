# Out-of-tree Makefile (may fail link if BST doorbell symbols are not exported).
# Preferred: ./install-into-kernel.sh /home/bst/workspace/linux-6.6
#
#   make KDIR=~/workspace/linux-6.6 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m += pci_epf_infer.o
obj-m += infer_rc.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KDIR_ABS := $(abspath $(KDIR))
SRC_ABS := $(abspath $(src))

# For out-of-tree only: fake relative include by adding a symlink step in build.sh
ccflags-y += -I$(src)

.PHONY: all modules clean inferlat check-version

all: check-version modules inferlat

check-version:
	@rel=$$(make -s -C $(KDIR_ABS) ARCH=$(ARCH) kernelrelease); \
	echo "kernelrelease=$$rel"; \
	if [ "$$rel" != "6.6.64-rt47" ]; then \
		echo "WARNING: want 6.6.64-rt47, got $$rel"; \
	fi

modules:
	@echo "NOTE: prefer ./install-into-kernel.sh for EP module (doorbell symbols)."
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

inferlat: inferlat.c infer_proto.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o inferlat inferlat.c

clean:
	$(MAKE) -C $(KDIR_ABS) M=$(CURDIR) clean 2>/dev/null || true
	rm -f inferlat
