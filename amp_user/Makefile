CC = arm-linux-gnueabihf-gcc

TARGET = user_amp

SRCS = \
	user_amp_main.c \
	user_amp_tun.c \
	user_amp_gateway.c \
	user_amp_batch.c \
	user_amp_datapath.c \
	user_amp_control.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -I. -I/usr/arm-linux-gnueabihf/include
LDFLAGS = -lpthread -lpcap

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
