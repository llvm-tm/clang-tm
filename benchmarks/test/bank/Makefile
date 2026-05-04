# Bank Benchmark Makefile - Modern C++17 Version
# Builds and tests the bank benchmark with different STM runtimes

# Paths
LLVM_PLUGIN_DIR := ../../../llvm_tm_plugin
BACKENDS_DIR := ../../../backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
TL2_DIR := $(BACKENDS_DIR)/TL2
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM
SWISSSTM_DIR := $(BACKENDS_DIR)/SwissTM

# Output directories
OUT_DIR := out
BIN_DIR := bin

# Compiler settings
CXX := clang++
CXXFLAGS := -std=c++17 -O1 -g -fno-inline -fno-inline-functions -pthread

# Plugin
PLUGIN_LIB := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# Source files
BANK_SRC := bank.cpp

# Build targets for each runtime
TARGETS := bank_uninstrumented bank_singlelock bank_tl2 bank_tinystm bank_swiss

.PHONY: all clean test help

all: $(OUT_DIR) $(BIN_DIR) $(TARGETS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Uninstrumented baseline (no TM, direct shared memory)
bank_uninstrumented: $(BANK_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(BANK_SRC) -o $(BIN_DIR)/$@ -pthread

# Build LLVM IR from modern C++ source (only once, shared for all runtimes)
$(OUT_DIR)/bank.bc: $(BANK_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -g -fno-inline -fno-inline-functions -emit-llvm -c $(BANK_SRC) \
	  -o $@ -fno-stack-protector -pthread

# Apply LLVM plugin instrumentation for each runtime
$(OUT_DIR)/bank_singlelock.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/bank_debug.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/bank_tl2.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/bank_tinystm.opt.bc: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

# Build with SingleGlobalLock runtime
bank_singlelock: $(OUT_DIR)/bank_singlelock.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_singlelock.opt.bc \
	  $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp -o $(BIN_DIR)/$@

# Build with test runtime (prints all TM calls)
bank_test: $(OUT_DIR)/bank_debug.opt.bc src/tm_test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_debug.opt.bc \
	  src/tm_test_runtime.cpp -o $(BIN_DIR)/$@

# Build with TL2 runtime
bank_tl2: $(OUT_DIR)/bank_tl2.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_tl2.opt.bc \
	  $(RUNTIMES_DIR)/TL2_runtime.cpp -o $(BIN_DIR)/$@

# Build with TinySTM runtime
bank_tinystm: $(OUT_DIR)/bank_tinystm.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL $(OUT_DIR)/bank_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $(BIN_DIR)/$@

# Build with SwissTM runtime
bank_swiss: $(OUT_DIR)/bank_tinystm.opt.bc $(RUNTIMES_DIR)/SwissTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/SwissTM_runtime.cpp -I $(SWISSSTM_DIR) -o $(BIN_DIR)/$@

# Test targets
test: test_tl2 test_tinystm test_swiss

test_tl2: bank_tl2
	@echo "Testing bank benchmark with TL2 (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tl2 -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

test_tinystm: bank_tinystm
	@echo "Testing bank benchmark with TinySTM (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tinystm -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

test_swiss: bank_swiss
	@echo "Testing bank benchmark with SwissTM (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_swiss -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)

help:
	@echo "Bank Benchmark Makefile (Modern C++17 Version)"
	@echo "=============================================="
	@echo ""
	@echo "Targets:"
	@echo "  all                - Build all benchmarks"
	@echo "  bank_uninstrumented - Baseline (no TM instrumentation)"
	@echo "  bank_singlelock    - SingleGlobalLock runtime"
	@echo "  bank_tl2          - Build bank benchmark with TL2 runtime"
	@echo "  bank_tinystm      - Build bank benchmark with TinySTM runtime"
	@echo "  bank_swiss        - Build bank benchmark with SwissTM runtime"
	@echo "  test              - Run all tests"
	@echo "  test_tl2          - Test bank benchmark with TL2"
	@echo "  test_tinystm      - Test bank benchmark with TinySTM"
	@echo "  test_swiss        - Test bank benchmark with SwissTM"
	@echo "  clean             - Clean build artifacts"
	@echo "  help              - Show this help message"
	@echo ""
	@echo "Bank program arguments:"
	@echo "  -d <ms>          Duration in milliseconds (default: 10000)"
	@echo "  -a <n>           Number of accounts (default: 1024)"
	@echo "  -t <n>           Number of threads (default: 4)"
	@echo "  -r <pct>         Percentage read-all transactions (default: 20)"
	@echo "  -w <pct>         Percentage write-all transactions (default: 0)"
	@echo "  --disjoint       Use disjoint account access per thread"

