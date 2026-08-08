CC = /home/fmsh/FMQLMP-Linux-SDK-Prj-20250408/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc

TARGET = user_amp

SRCS = \
	user_main.c \
	user_gateway.c \
	user_datapath.c \
	user_control.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -I.
# -I/usr/arm-linux-gnueabihf/include
LDFLAGS = -lpthread

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
