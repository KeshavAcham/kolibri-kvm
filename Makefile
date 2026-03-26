CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS = -lm

.PHONY: all test kvm clean

all: test

test: main.c jvm.c jvm.h          # tests only, no classloader
	$(CC) $(CFLAGS) -o kvm_test main.c jvm.c $(LDFLAGS)
	./kvm_test

kvm: classloader_demo.c jvm.c classfile.c jvm.h classfile.h   # ADD classfile.c
	$(CC) $(CFLAGS) -o kvm classloader_demo.c jvm.c classfile.c $(LDFLAGS)

clean:
	rm -f kvm_test kvm *.o
