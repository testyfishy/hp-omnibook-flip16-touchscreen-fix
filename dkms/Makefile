# Out-of-tree build of i2c-hid-acpi with the HP OmniBook X Flip 16 (8DA0/8DA1) touch-INT routing quirk.
# The stock in-tree i2c-hid core module is used unchanged.
obj-m := i2c-hid-acpi.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
