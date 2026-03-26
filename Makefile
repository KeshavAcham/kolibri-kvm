CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g
LDFLAGS = -lm

# issue #11: -lm is required for fmodf, isnan from <math.h>

.PHONY: all test clean

all: test

test: main.c jvm.c jvm.h
	$(CC) $(CFLAGS) -o kvm_test main.c jvm.c $(LDFLAGS)
	./kvm_test

kvm: classloader_demo.c jvm.c jvm.h
	$(CC) $(CFLAGS) -o kvm classloader_demo.c jvm.c $(LDFLAGS)

clean:
	rm -f kvm_test kvm *.o
