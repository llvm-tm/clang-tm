CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

# The workloads use the Arm ACLE TME intrinsics (__tstart / __tcommit /
# __tsend), and arm_acle.h declares those ONLY when the target defines
# __ARM_FEATURE_TME.  Two consequences worth knowing before the first build:
#
#   * `-march=armv8.1-a` is NOT enough — TME is an optional feature of that
#     architecture version and has to be switched on explicitly, hence +tme.
#   * clang/LLVM (checked through LLVM 22) has no Arm TME support at all: its
#     AArch64 feature list has no "tme" and __ARM_FEATURE_TME is never defined,
#     so this directory needs a GCC >= 10 cross-compiler:
#         sudo apt install gcc-aarch64-linux-gnu
#         make CROSS_COMPILE=aarch64-linux-gnu-
MARCH ?= armv8.1-a+tme
CFLAGS = -Wall -O2 -g -march=$(MARCH) -pthread
LDFLAGS = -lm -pthread

GEM5_ROOT = ../../gem5
M5OPS_DIR = $(GEM5_ROOT)/util/m5/build/aarch64/out
M5OPS_INC = $(GEM5_ROOT)/include

BINARIES = array_sum hash_table

# Evaluated once: does $(CC) actually enable TME for $(MARCH)?
TME_OK := $(shell $(CC) -march=$(MARCH) -dM -E -x c /dev/null 2>/dev/null \
                  | grep -c __ARM_FEATURE_TME)

.PHONY: all clean check-tme

all: check-tme $(BINARIES)

# Fail with the reason instead of a screenful of "implicit declaration of
# __tstart" errors, which is what a missing +tme (or clang) looks like.
check-tme:
	@if [ "$(TME_OK)" -lt 1 ] 2>/dev/null; then \
		echo "error: $(CC) does not define __ARM_FEATURE_TME with -march=$(MARCH)."; \
		echo "  These workloads need a TME-capable AArch64 GCC (>= 10), e.g."; \
		echo "  sudo apt install gcc-aarch64-linux-gnu && make CROSS_COMPILE=aarch64-linux-gnu-"; \
		echo "  (clang has no Arm TME support as of LLVM 22.)"; \
		exit 1; \
	fi

array_sum: array_sum.c ../common/htm_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

hash_table: hash_table.c ../common/htm_bench.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

%: %.c
	$(CC) $(CFLAGS) -I$(M5OPS_INC) -o $@ $< -L$(M5OPS_DIR) -lm5 $(LDFLAGS)

clean:
	rm -f $(BINARIES)
