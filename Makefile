CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g

# test runner (your original main.c)
test: main.o jvm.o
	$(CC) $(CFLAGS) -o $@ $^

# class loader demo
kvm: classloader_demo.o jvm.o classfile.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o test kvm

.PHONY: clean
