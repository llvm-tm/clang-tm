# ── GPU benchmarks ──────────────────────────────────────────────
#
# Builds standalone GPU TM benchmarks (no LLVM plugin).  Transaction
# bodies are device functions that call the CSMV GPU API directly via
# the batch executor.
#
# Requires either CUDA (nvcc) or HIP (hipcc).
#
# Usage:
#   make            # auto-detect nvcc/hipcc
#   make CUDA=/path/to/nvcc
#   make HIP=1 HIPCC=/path/to/hipcc
#   make run        # run gpu_ycsb with defaults
#   make clean

# ── Toolchain detection ────────────────────────────────────────
HIP     ?= 0
CUDA    ?= $(shell command -v nvcc 2>/dev/null)
HIPCC   ?= $(shell command -v hipcc 2>/dev/null)

ifeq ($(HIP),1)
  ifeq ($(HIPCC),)
    $(error HIP build requested but hipcc not found. Set HIPCC=/path/to/hipcc)
  endif
  COMPILER := $(HIPCC)
  ARCH_FLAG := --offload-arch=native
else
  ifeq ($(CUDA),)
    $(error nvcc not found. Install CUDA and set CUDA=/path/to/nvcc, or use HIP=1)
  endif
  COMPILER := $(CUDA)
  ARCH_FLAG := -arch=sm_90
endif

# ── Paths ──────────────────────────────────────────────────────
ROOT    := ../..
CSMV_DIR:= $(ROOT)/backends/tm_impl/csmv
COMMON  := $(ROOT)/backends/tm_impl/common
INCS    := -I$(CSMV_DIR)/include -I$(CSMV_DIR)/gpu -I$(COMMON)
BIN     := bin

# ── Targets ────────────────────────────────────────────────────
BINARIES := $(BIN)/gpu_ycsb $(BIN)/gpu_kmeans $(BIN)/gpu_memcached $(BIN)/gpu_tpcc

all: $(BINARIES)

$(BIN)/gpu_ycsb: gpu_ycsb.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
                 $(CSMV_DIR)/gpu/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(ARCH_FLAG) $(INCS) \
	    gpu_ycsb.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_kmeans: gpu_kmeans.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
                   $(CSMV_DIR)/gpu/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(ARCH_FLAG) $(INCS) \
	    gpu_kmeans.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_memcached: gpu_memcached.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
                      $(CSMV_DIR)/gpu/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(ARCH_FLAG) $(INCS) \
	    gpu_memcached.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_tpcc: gpu_tpcc.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
                 $(CSMV_DIR)/gpu/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(ARCH_FLAG) $(INCS) \
	    gpu_tpcc.cu $(CSMV_DIR)/gpu/csmv_batch_executor.cu \
	    -o $@

run: all
	$(BIN)/gpu_ycsb 4096 1024 8 50
	$(BIN)/gpu_kmeans 4096 2 8 1024
	$(BIN)/gpu_memcached 4096 1024 50
	$(BIN)/gpu_tpcc 8 4 64 1024 10

.PHONY: all run clean
clean:
	rm -rf $(BIN)
