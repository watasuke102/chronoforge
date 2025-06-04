MODULE_NAME := kmodule
KERNEL_DIR := /lib/modules/$(shell uname -r)/build
BUILD_DIR  ?= $(PWD)/build

obj-m := $(MODULE_NAME).o

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/Makefile
	$(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) src=$(PWD) modules
$(BUILD_DIR)/Makefile: $(BUILD_DIR)
	touch $@
$(BUILD_DIR):
	mkdir -p $@

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) src=$(PWD) clean
install: $(BUILD_DIR)/$(MODULE_NAME).ko
	sudo insmod $<
uninstall:
	sudo rmmod $(MODULE_NAME)
