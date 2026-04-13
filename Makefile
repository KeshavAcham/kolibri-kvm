CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -g
TARGET  = kvm
SRCS    = jvm.c main.c

$(TARGET): $(SRCS) jvm.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: clean
