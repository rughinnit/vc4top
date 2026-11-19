# DKMS build for the vc4mon counter-enabling module.
#
# DKMS copies this directory to /var/lib/dkms/vc4top/<version>/build and runs
# the MAKE[0] command from dkms.conf.  The target kernel is always the running
# one (postinst builds with -k $(uname -r), DKMS autoinstall rebuilds at boot),
# so KDIR defaults to the running kernel's build tree.  Override KDIR to build
# for another tree by hand.
obj-m := vc4mon.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
