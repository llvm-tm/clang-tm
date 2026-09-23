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

# Detect the installed GPU's compute capability (via nvidia-smi) to pick a
# valid -arch for *this* machine.  Override with ARCH=sm_XX if desired.
# sm_90 (H100) is NOT portable: this repo has been run on GTX 10xx (sm_61),
# RTX 20xx (sm_75), etc.  A sm_90 binary will fail to load on older GPUs.
SMID    ?= $(shell nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n1 | tr -d '.')
ifeq ($(SMID),)
  SMID = 90
endif
ARCH    ?= sm_$(SMID)

# C++ standard library header fix (HIP path only; nvcc uses host gcc and does
# not understand --gcc-install-dir).  ROCm's clang prefers the gcc-16 libstdc++
# headers, but only 15/12 -dev packages may be installed, causing
# "Could not find standard C++ header 'cmath'".  Mirror the fix that
# benchmarks/cpp/Makefile applies for the clang++ toolchain.  See review-03.
STDLIB_FLAG :=

ifeq ($(HIP),1)
  ifeq ($(HIPCC),)
    $(error HIP build requested but hipcc not found. Set HIPCC=/path/to/hipcc)
  endif
  COMPILER := $(HIPCC)
  ARCH_FLAG := --offload-arch=native
  ifeq ($(wildcard /usr/include/c++/16),)
    ifneq ($(wildcard /usr/lib/gcc/x86_64-linux-gnu/15),)
      STDLIB_FLAG := --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15
    else ifneq ($(wildcard /usr/lib/gcc/x86_64-linux-gnu/12),)
      STDLIB_FLAG := --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12
    endif
  endif
else
  ifeq ($(CUDA),)
    $(error nvcc not found. Install CUDA and set CUDA=/path/to/nvcc, or use HIP=1)
  endif
  COMPILER := $(CUDA)
  ARCH_FLAG := -arch=$(ARCH)
endif

# ── Paths ──────────────────────────────────────────────────────
ROOT    := ../..
CSMV_DIR:= $(ROOT)/gpu/backends/csmv
CSMV_CU := $(CSMV_DIR)/cuda
CSMV_INC:= $(CSMV_DIR)/include
CSMV_API:= $(ROOT)/backends/tm_impl/csmv/include
GUST_DIR:= $(ROOT)/gpu/backends/gpu_gust/cuda
GUST_API:= $(ROOT)/gpu/backends/gpu_gust/include
COMMON  := $(ROOT)/backends/tm_impl/common
INCS    := -I$(CSMV_API) -I$(CSMV_INC) -I$(CSMV_CU) -I$(GUST_DIR) -I$(GUST_API) -I$(COMMON)
GPU_STM_CU := $(ROOT)/gpu/backends/gpu_stm/cuda
GPU_STM_API:= $(ROOT)/backends/tm_impl/gpu_stm/include
CSMV_API := $(ROOT)/backends/tm_impl/csmv/include
GPUTX_CU := $(ROOT)/gpu/backends/gpu_gputx/cuda
GPUTX_INC:= $(ROOT)/gpu/backends/gpu_gputx/include
GACCO_CU := $(ROOT)/gpu/backends/gpu_gacco/cuda
GACCO_INC:= $(ROOT)/gpu/backends/gpu_gacco/include
BIN     := bin

# RDC flag differs: nvcc uses -rdc=true, hipcc uses -fgpu-rdc
ifeq ($(HIP),1)
  RDC_FLAG := -fgpu-rdc
else
  RDC_FLAG := -rdc=true
endif

# ── Targets ────────────────────────────────────────────────────
BINARIES := $(BIN)/gpu_ycsb $(BIN)/gpu_kmeans $(BIN)/gpu_memcached $(BIN)/gpu_tpcc $(BIN)/gpu_fuzz_counter
GUST_BINARIES := $(BIN)/gpu_bank $(BIN)/gpu_ycsb_gust $(BIN)/gpu_memcached_gust $(BIN)/gpu_gust_smoke

all: $(BINARIES) $(GUST_BINARIES)

$(BIN)/gpu_fuzz_counter: gpu_fuzz_counter.cu $(CSMV_CU)/csmv_batch_executor.cu \
                         $(CSMV_INC)/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_fuzz_counter.cu $(CSMV_CU)/csmv_batch_executor.cu \
	    -o $@


$(BIN)/gpu_ycsb: gpu_ycsb.cu $(CSMV_CU)/csmv_batch_executor.cu \
                 $(CSMV_INC)/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_ycsb.cu $(CSMV_CU)/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_kmeans: gpu_kmeans.cu $(CSMV_CU)/csmv_batch_executor.cu \
                   $(CSMV_INC)/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_kmeans.cu $(CSMV_CU)/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_memcached: gpu_memcached.cu $(CSMV_CU)/csmv_batch_executor.cu \
                      $(CSMV_INC)/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_memcached.cu $(CSMV_CU)/csmv_batch_executor.cu \
	    -o $@

$(BIN)/gpu_tpcc: gpu_tpcc.cu $(CSMV_CU)/csmv_batch_executor.cu \
                 $(CSMV_INC)/csmv_batch_executor.hpp
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_tpcc.cu $(CSMV_CU)/csmv_batch_executor.cu \
	    -o $@

# ── GUST-bound benchmarks (one tx per lane, warp-cooperative commit) ──
GUST_EXEC := $(GUST_DIR)/gpu_gust_batch_executor.cu
GUST_CUH  := $(GUST_API)/gpu_gust_batch_executor.cuh

$(BIN)/gpu_bank: gpu_bank.cu $(GUST_EXEC) $(GUST_CUH)
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_bank.cu $(GUST_EXEC) -o $@

$(BIN)/gpu_ycsb_gust: gpu_ycsb_gust.cu $(GUST_EXEC) $(GUST_CUH)
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_ycsb_gust.cu $(GUST_EXEC) -o $@

$(BIN)/gpu_memcached_gust: gpu_memcached_gust.cu $(GUST_EXEC) $(GUST_CUH)
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_memcached_gust.cu $(GUST_EXEC) -o $@

# gpu_gust_smoke.cu lives HERE (review-05 layout pass): a benchmark/test
# driver belongs in gpu/benchmarks/, not inside the backend.
$(BIN)/gpu_gust_smoke: gpu_gust_smoke.cu $(GUST_EXEC) $(GUST_CUH)
	@mkdir -p $(BIN)
	$(COMPILER) -std=c++17 $(RDC_FLAG) $(ARCH_FLAG) $(STDLIB_FLAG) $(INCS) \
	    gpu_gust_smoke.cu $(GUST_EXEC) -o $@

run: all
	$(BIN)/gpu_ycsb 4096 1024 8 50
	$(BIN)/gpu_kmeans 4096 2 8 1024
	$(BIN)/gpu_memcached 4096 1024 50
	$(BIN)/gpu_tpcc 8 4 64 1024 10
	$(BIN)/gpu_fuzz_counter 1024 4
	$(BIN)/gpu_gust_smoke 4 256 1000
	$(BIN)/gpu_bank 32 1024 1000 0
	$(BIN)/gpu_bank 32 1024 1000 90
	$(BIN)/gpu_ycsb_gust 4096 1024 8 50
	$(BIN)/gpu_memcached_gust 4096 1024 50

# ── Reference-kernel compile check (review-05 G-08) ─────────────
# These device/host files are reference implementations not linked into the
# shipped benchmark binaries. Keeping them compiling on the real toolchain
# (nvcc or hipcc) prevents "half-broken reference kernels" from drifting.
KERNEL_CHECK_CU := \
    $(CSMV_CU)/csmv_kernel.cu \
    $(GPU_STM_CU)/pr_stm_runtime.cu \
    $(GUST_DIR)/gpu_gust_runtime.cu \
    $(GPUTX_CU)/gpu_gputx_runtime.cu \
    $(GACCO_CU)/gpu_gacco_runtime.cu
KERNEL_CHECK_CPP := \
    $(GPU_STM_CU)/pr_stm_host.cpp \
    $(GUST_DIR)/gpu_gust_host.cpp \
    $(GPUTX_CU)/gpu_gputx_host.cpp \
    $(GACCO_CU)/gpu_gacco_host.cpp

# Host-side files compile as plain C++; tm_gpu_platform.hpp needs the HIP
# platform macros that hipcc injects only for device compilation.
HIPROOT ?= /opt/rocm
ifeq ($(HIP),1)
  KERNEL_HOSTDEFS := -D__HIP_PLATFORM_AMD__ -DTM_GPU_USE_HIP \
      $(if $(wildcard $(HIPROOT)/include),-I$(HIPROOT)/include)
else
  KERNEL_HOSTDEFS :=
endif

KERNEL_INCS := $(INCS) -I$(GPU_STM_API) -I$(GPUTX_INC) -I$(GACCO_INC) \
    -I$(ROOT)/backends/tm_impl -I$(ROOT)/backends/tm_impl/tm_region_allocator \
    -I$(ROOT)/explicit_api/cpp

kernel-check:
	@mkdir -p $(BIN)/objcheck
	@for src in $(KERNEL_CHECK_CU); do \
	    name=$$(basename $$src); \
	    printf 'kernel-check %-28s ' $$name; \
	    $(COMPILER) -std=c++17 $(ARCH_FLAG) $(STDLIB_FLAG) $(KERNEL_INCS) \
	        -c $$src -o $(BIN)/objcheck/$$name.o \
	        || { echo FAIL; exit 1; }; \
	    echo OK; \
	done
	@for src in $(KERNEL_CHECK_CPP); do \
	    name=$$(basename $$src); \
	    printf 'kernel-check %-28s ' $$name; \
	    $(COMPILER) -std=c++17 $(STDLIB_FLAG) $(KERNEL_HOSTDEFS) $(KERNEL_INCS) \
	        -c -x c++ $$src -o $(BIN)/objcheck/$$name.o \
	        || { echo FAIL; exit 1; }; \
	    echo OK; \
	done

.PHONY: all run clean kernel-check
clean:
	rm -rf $(BIN)
