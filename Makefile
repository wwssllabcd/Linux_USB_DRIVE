#
# Makefile for kernel test
#

PWD         := $(shell pwd) 
KVERSION    := $(shell uname -r)
KERNEL_DIR   = /usr/src/linux-headers-$(KVERSION)/

MODULE_NAME  = eric_usb_driver
#MODULE_NAME  = usb-skeleton

obj-m       := $(MODULE_NAME).o   

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules
	- rmmod $(MODULE_NAME).ko  
	- rmmod usb_storage
	insmod $(MODULE_NAME).ko  
	$(PRINT_MESSAGE) 
clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
	rmmod $(MODULE_NAME).ko  
	$(PRINT_MESSAGE)  


define PRINT_MESSAGE
	@echo 
	@echo ------message--------
	@dmesg|tail
endef
