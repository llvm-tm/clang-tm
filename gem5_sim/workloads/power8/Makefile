CROSS_COMPILE ?= powerpc64le-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -O2 -mcpu=power8 -mhtm -pthread
LDFLAGS = -lm -pthread

# For bare-metal builds (avoids glibc VSX/VMX instructions not in gem5)
BM_CFLAGS = -Wall -O2 -mcpu=power8 -mhtm
BM_LDFLAGS = -nostdlib -nostartfiles

BINARIES = array_sum array_sum_bench htm_test tbegin_tend

.PHONY: all clean

all: $(BINARIES)

# glibc-linked binary (may hit unimplemented VSX/VMX instructions in gem5)
array_sum: array_sum.c ../common/htm_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# bare-metal binary (uses raw syscalls, no glibc dependency)
array_sum_bench: array_sum_bench.c
	$(CC) $(BM_CFLAGS) $(BM_LDFLAGS) -o $@ $<

%.o: %.S
	$(CC) $(BM_CFLAGS) $(BM_LDFLAGS) -c -o $@ $<

tbegin_tend: tbegin_tend.S
	$(CC) $(BM_CFLAGS) $(BM_LDFLAGS) -o $@ $<

htm_test: htm_test.S
	$(CC) $(BM_CFLAGS) $(BM_LDFLAGS) -o $@ $<

clean:
	rm -f $(BINARIES) *.o
