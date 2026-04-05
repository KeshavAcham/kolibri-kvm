# kolibri-kvm — Makefile
CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET  = kvm
SRCS    = main.c jvm.c classfile.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c jvm.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	./$(TARGET)

.PHONY: all clean test
