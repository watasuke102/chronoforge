MODULE_NAME := kmodule
KERNEL_DIR  ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR   ?= $(PWD)/build
MODULE_KEY  ?= /var/lib/shim-signed/mok/MOK.priv
MODULE_X509 ?= /var/lib/shim-signed/mok/MOK.der

obj-m := $(MODULE_NAME).o

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/$(MODULE_NAME).ko

$(BUILD_DIR)/$(MODULE_NAME).ko: $(BUILD_DIR)/Makefile
	$(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) src=$(PWD) modules
$(BUILD_DIR)/Makefile: $(BUILD_DIR)
	touch $@
$(BUILD_DIR):
	mkdir -p $@

/dev/$(MODULE_NAME):
	sudo mknod $@ c $(shell cat /proc/devices | grep $(MODULE_NAME) | awk '{print $$1}') 0
	sudo chmod 666 $@

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) src=$(PWD) clean
install: $(BUILD_DIR)/$(MODULE_NAME).ko
	sudo kmodsign sha512 $(MODULE_KEY) $(MODULE_X509) $<
	sudo $(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) src=$(PWD) modules_install
	sudo depmod -A
	sudo insmod $<
uninstall:
	sudo rm -f /dev/$(MODULE_NAME)
	sudo rmmod $(MODULE_NAME)

