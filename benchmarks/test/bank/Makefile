# Bank Benchmark Makefile - Modern C++17 Version
# Builds and tests the bank benchmark with different STM runtimes

# Paths
LLVM_PLUGIN_DIR := ../../../llvm_tm_plugin
BACKENDS_DIR := ../../../backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
TL2_DIR := $(BACKENDS_DIR)/tl2
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM

# Output directories
OUT_DIR := out
BIN_DIR := bin

# Compiler settings
CXX := clang++
CXXFLAGS := -std=c++17 -O1 -fno-inline -fno-inline-functions -pthread

# Plugin
PLUGIN_LIB := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# TL2 library
TL2_LIB := $(TL2_DIR)/lib/libtl2.a

# TinySTM library
TINYSTM_LIB := $(TINYSTM_DIR)/lib/libstm.a

# Source files
BANK_SRC := bank.cpp

# Build targets for each runtime
TARGETS := bank_tl2 bank_tinystm

.PHONY: all clean test help

all: $(OUT_DIR) $(BIN_DIR) $(TARGETS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Build LLVM IR from modern C++ source (only once, shared for all runtimes)
$(OUT_DIR)/bank.bc: $(BANK_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(BANK_SRC) \
	  -o $@ -fno-stack-protector -pthread

# Apply LLVM plugin instrumentation for each runtime
$(OUT_DIR)/bank_tl2.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/bank_tinystm.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

# Build with TL2 runtime
bank_tl2: $(OUT_DIR)/bank_tl2.opt.bc $(TL2_LIB) $(RUNTIMES_DIR)/tl2_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_tl2.opt.bc \
	  $(RUNTIMES_DIR)/tl2_runtime.cpp $(TL2_LIB) -o $(BIN_DIR)/$@ \
	  -I$(TL2_DIR) -L$(TL2_DIR)/lib -ltl2

# Build with TinySTM runtime
bank_tinystm: $(OUT_DIR)/bank_tinystm.opt.bc $(TINYSTM_LIB) $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/TinySTM_runtime.cpp $(TINYSTM_LIB) -o $(BIN_DIR)/$@ \
	  -I$(TINYSTM_DIR)/include -L$(TINYSTM_DIR)/lib -lstm

# Test targets
test: test_tl2 test_tinystm

test_tl2: bank_tl2
	@echo "Testing bank benchmark with TL2 (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tl2 -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

test_tinystm: bank_tinystm
	@echo "Testing bank benchmark with TinySTM (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tinystm -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)

help:
	@echo "Bank Benchmark Makefile (Modern C++17 Version)"
	@echo "=============================================="
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all benchmarks"
	@echo "  bank_tl2         - Build bank benchmark with TL2 runtime"
	@echo "  bank_tinystm     - Build bank benchmark with TinySTM runtime"
	@echo "  test             - Run all tests"
	@echo "  test_tl2         - Test bank benchmark with TL2"
	@echo "  test_tinystm     - Test bank benchmark with TinySTM"
	@echo "  clean            - Clean build artifacts"
	@echo "  help             - Show this help message"
	@echo ""
	@echo "Bank program arguments:"
	@echo "  -d <ms>          Duration in milliseconds (default: 10000)"
	@echo "  -a <n>           Number of accounts (default: 1024)"
	@echo "  -t <n>           Number of threads (default: 4)"
	@echo "  -r <pct>         Percentage read-all transactions (default: 20)"
	@echo "  -w <pct>         Percentage write-all transactions (default: 0)"
	@echo "  --disjoint       Use disjoint account access per thread"

