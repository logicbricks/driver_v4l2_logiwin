
TARGET=app_fgtest_linux

CC=arm-none-linux-gnueabi-gcc
LIBS=-lpthread

ifeq ($(DEBUG),1)
	CFLAGS=-g3 -O0 -Wall
else
	CFLAGS=-O2 -Wall
endif

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)
HDRS=$(wildcard *.h)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	@echo "$(OBJS)"
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f *.o $(TARGET)
