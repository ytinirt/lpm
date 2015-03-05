#cflags := -Wall -fprofile-arcs -ftest-coverage
cflags := -Wall

default: lpm

lpm: lpm.c
	gcc $(cflags) -c lpm.c -o lpm.o

clean:
	rm -rf *.o
