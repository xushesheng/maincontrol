KERNEL_SRC = /home/fmsh/FMQL-Linux-SDK-Prj-20230801_1113/linux-4.14.55-fmsh
CROSS_COMPILE = /home/fmsh/FMQL-Linux-SDK-Prj-20230801_1113/gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

obj-m += driver_amp.o

driver_amp-objs := \
	driver_amp_main.o \
	driver_amp_dev.o \
	driver_amp_txrx.o \
	driver_amp_hw.o

all:
	make ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(CURDIR) modules

clean:
	make ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(CURDIR) clean
