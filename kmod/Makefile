# SPDX-License-Identifier: GPL-2.0
#
# Build the kernel module and the userspace test program.
#   make            - build both
#   make module     - just valdev.ko (needs kernel headers: linux-headers-$(uname -r))
#   make test       - just test_valdev
#   make check      - load the module, run the tests, unload (needs root; use a throwaway VM)

obj-m += valdev.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all module test check clean

all: module test

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

test: test_valdev

test_valdev: test_valdev.c valdev_ioctl.h
	$(CC) -O2 -g -Wall -Wextra -Werror -pthread -o $@ test_valdev.c

check: all
	./run_in_vm.sh

clean:
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f test_valdev
