# SPDX-License-Identifier: GPL-2.0-only
# Kbuild makefile for the guest kernel module.
#
# The module must be built against the *guest* kernel, not the machine you
# are sitting at. scripts/build-kmod.sh picks the right headers (and can
# run this inside the container defined in containers/); building here
# directly only makes sense when KDIR already points at them.
#
#   make -C guest/kmod KDIR=/lib/modules/6.8.0-134-generic/build
obj-m += ivshmem_rproto.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
