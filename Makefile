#
# Makefile for kernel test
#

#Makefile ���i�H�ϥ��ܼơA�@���ܼƤj�g�A�b�ޥ��ܼƮɡA�ĥΤp�A���X�_�ܼƦW�e�[�]$�^�Ÿ��ӥΡC
#pwd = �ثe���ؿ�
PWD         := $(shell pwd) 
KVERSION    := $(shell uname -r)
KERNEL_DIR   = /usr/src/linux-headers-$(KVERSION)/

MODULE_NAME  = hello

#obj-m��ܻݭn�sö���Ҳժ��ؼ��ɮצW���X
obj-m       := $(MODULE_NAME).o   

all:
	# -C���kernel source�ؿ��A�]���ڭ̦��ѷ�header�A�ҥH�o��n�i�D make �h����
	# Use make M=dir to specify directory of external module to build�A�w�q�b kernel ��
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
