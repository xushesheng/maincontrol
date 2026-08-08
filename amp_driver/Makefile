KERNEL_SRC = /home/fmsh/FMQLMP-Linux-SDK-Prj-20250408/linux-5.4.52-fmsh
CROSS_COMPILE = /home/fmsh/FMQLMP-Linux-SDK-Prj-20250408/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-

obj-m += driver_amp.o

driver_amp-objs := \
	driver_main.o \
	driver_txrx.o \
	driver_map.o

all:
	make ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(CURDIR) modules

clean:
	make ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(CURDIR) clean
