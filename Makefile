#
# Makefile for kernel test
#

#Makefile 中可以使用變數，一般變數大寫，在引用變數時，採用小括弧擴起變數名前加（$）符號來用。
#pwd = 目前的目錄
PWD         := $(shell pwd) 
KVERSION    := $(shell uname -r)
KERNEL_DIR   = /usr/src/linux-headers-$(KVERSION)/

MODULE_NAME  = hello

#obj-m表示需要編繹成模組的目標檔案名集合
obj-m       := $(MODULE_NAME).o   

all:
	# -C表示kernel source目錄，因為我們有參照header，所以這邊要告訴 make 去哪找
	# Use make M=dir to specify directory of external module to build，定義在 kernel 中
	make -C $(KERNEL_DIR) M=$(PWD) modules
	#insmod $(MODULE_NAME).ko
	#$(PRINT_MESSAGE)
clean:
	#rmmod $(MODULE_NAME)
	make -C $(KERNEL_DIR) M=$(PWD) clean
	#$(PRINT_MESSAGE)
   
define PRINT_MESSAGE
    @echo --------Message--------
    @dmesg | tail
endef
