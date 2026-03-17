CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g
TARGET  = kvm

all: $(TARGET)

$(TARGET): jvm.c main.c jvm.h
	$(CC) $(CFLAGS) -o $(TARGET) jvm.c main.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
 
