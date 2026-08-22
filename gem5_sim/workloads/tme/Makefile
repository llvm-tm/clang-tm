CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -O2 -g -march=armv8.1-a -pthread
LDFLAGS = -lm -pthread

GEM5_ROOT = ../../gem5
M5OPS_DIR = $(GEM5_ROOT)/util/m5/build/aarch64/out
M5OPS_INC = $(GEM5_ROOT)/include

BINARIES = array_sum hash_table

.PHONY: all clean

all: $(BINARIES)

array_sum: array_sum.c ../common/htm_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

hash_table: hash_table.c ../common/htm_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

%: %.c
	$(CC) $(CFLAGS) -I$(M5OPS_INC) -o $@ $< -L$(M5OPS_DIR) -lm5 $(LDFLAGS)

clean:
	rm -f $(BINARIES)
