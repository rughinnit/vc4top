# Out-of-tree build for the vc4mon counter-enabling module.
#
#   make -C kernel
#   sudo make -C kernel install
#
# install copies vc4mon.ko to /lib/modules/$(uname -r)/extra and runs depmod.
# Load it with `sudo modprobe vc4mon` (or /etc/modules-load.d/vc4mon.conf).

obj-m := vc4mon.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -D -m 0644 vc4mon.ko \
		/lib/modules/$(shell uname -r)/extra/vc4mon.ko
	depmod -a $(shell uname -r)

.PHONY: all clean install
