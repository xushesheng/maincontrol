CC = arm-linux-gnueabihf-gcc

TARGET = user_amp

SRCS = \
	user_main.c \
	user_gateway.c \
	user_datapath.c \
	user_control.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -I. -I/usr/arm-linux-gnueabihf/include
LDFLAGS = -lpthread -lpcap

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
