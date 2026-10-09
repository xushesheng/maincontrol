CC = /home/fmsh/FMQLMP-Linux-SDK-Prj-20250408/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc

TARGET = user_amp

SRCS = \
	user_main.c \
	user_gateway.c \
	user_datapath.c \
	user_bcast.c \
	user_portcfg.c \
	user_control.c \
	user_freqtable.c \
	cjson/cJSON.c \
	user_link.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -I.
# -I/usr/arm-linux-gnueabihf/include
LDFLAGS = -lpthread -lm
# -lm：cJSON 内部引用 fabs 等 math 符号（部分老 glibc 中 libm 未并入 libc，链接需要）

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
