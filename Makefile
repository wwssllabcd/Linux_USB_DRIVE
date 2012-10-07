#
# Makefile for kernel test
#


PWD         := $(shell pwd)
KVERSION    := $(shell uname -r)

MODULE_NAME  = hello
KERNEL_DIR  = /usr/src/linux-headers-$(KVERSION)/

obj-m       := $(MODULE_NAME).o   

all:
	make modules -C $(KERNEL_DIR) M=$(PWD) 
	#insmod $(MODULE_NAME).ko
	$(PRINT_MESSAGE)
clean:
	#rmmod $(MODULE_NAME)
	make -C $(KERNEL_DIR) M=$(PWD) clean
	$(PRINT_MESSAGE)
   
define PRINT_MESSAGE
    @echo --------Message--------
    @dmesg | tail
endef
