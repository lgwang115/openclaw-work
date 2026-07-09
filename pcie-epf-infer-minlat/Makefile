# Out-of-tree build for min-latency EP2 infer stack
#
# EP board / cross-build:
#   make KDIR=/path/to/linux-6.6 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- ep
#
# RC board / cross-build:
#   make KDIR=/path/to/linux-6.6 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- rc
#
# Userspace:
#   make inferlat

obj-m += pci_epf_infer.o
obj-m += infer_rc.o

# Point at BST doorbell header inside the kernel tree
ccflags-y += -I$(src) -I$(KDIR)/drivers/pci/controller/bst

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all ep rc modules clean inferlat

all: modules inferlat

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

ep:
	$(MAKE) -C $(KDIR) M=$(CURDIR) pci_epf_infer.ko

rc:
	$(MAKE) -C $(KDIR) M=$(CURDIR) infer_rc.ko

inferlat: inferlat.c infer_proto.h
	$(CROSS_COMPILE)gcc -O2 -Wall -o inferlat inferlat.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || true
	rm -f inferlat
