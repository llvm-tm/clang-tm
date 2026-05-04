# Makefile for STM backend tests
# Tests each backend independently without LLVM instrumentation

CXX = clang++
CXX = clang++
CXXFLAGS = -std=c++17 -O0 -pthread -g
LDFLAGS = -pthread

# Directories
BIN_DIR = bin
OUT_DIR = out
RUNTIME_DIR = ../runtimes

# Backend directories
TL2_DIR = ../TL2
TINYSTM_DIR = ../TinySTM
NOREC_DIR = ../NOrec
SWISS_DIR = ../SwissTM

# Include paths
TL2_INCLUDE = -I$(TL2_DIR) -I..
TINYSTM_INCLUDE = -I$(TINYSTM_DIR) -I..
SWISS_INCLUDE = -I$(SWISS_DIR) -I..

# Test sources
TEST_SOURCES = $(filter-out Makefile,$(wildcard test_*.cpp))

# Test names (without extension)
TESTS = $(patsubst test_%.cpp,%,$(TEST_SOURCES))

# Runtime tests mapping
RUNTIME_TESTS = runtime_simple runtime_simple2 runtime_simple_st runtime_swiss_simple

# TinySTM flavor runtimes
TINYSTM_WBETL = runtime_tinystm_wbetl
TINYSTM_WT = runtime_tinystm_wt
TINYSTM_WBCTL = runtime_tinystm_wbctl

.PHONY: all clean run run_all bin out tinystm_all

# Setup directories
all: out bin tinystm_all runtime_simple runtime_swiss_simple

tinystm_all: $(TINYSTM_WBETL) $(TINYSTM_WT) $(TINYSTM_WBCTL)

out:
	mkdir -p $(OUT_DIR)

bin:
	mkdir -p $(BIN_DIR)

# Build rules for runtime tests
runtime_simple: out bin
	@echo "Building runtime_simple with TinySTM (DESIGN_WBCTL)..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/runtime_simple test_runtime_simple.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

runtime_simple2: out bin
	@echo "Building runtime_simple2 with TinySTM (DESIGN_WBCTL)..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/runtime_simple2 test_runtime_simple2.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

### TODO: st test for all backends
runtime__st: out bin
	@echo "Building runtime_simple_st with TinySTM (DESIGN_WBCTL, single-threaded)..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/runtime_simple_st test_runtime___st.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

### TODO: st test for all backends
runtime__mt: out bin
	@echo "Building runtime_simple_st with TinySTM (DESIGN_WBCTL, multi-threaded)..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/runtime_simple_st test_runtime___st.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

runtime_swiss_simple: out bin
	@echo "Building runtime_swiss_simple with SwissTM..."
	$(CXX) $(CXXFLAGS) $(SWISS_INCLUDE) -o bin/runtime_swiss_simple test_swiss_simple_new.cpp $(RUNTIME_DIR)/SwissTM_runtime.cpp $(LDFLAGS)

# Jump-back test (tests sigsetjmp/siglongjmp instead of while loop)
runtime_jump_back: out bin
	@echo "Building runtime_jump_back..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -o bin/runtime_jump_back test_jump_back.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

# Generic compilation rules
test_%: test_%.cpp out bin
	@echo "Building $@..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/$@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

# Generate LLVM IR and bitcode - need explicit dependencies
out/test_%.ll: test_%.cpp out
	@echo "Generating LLVM IR: $@"
	$(CXX) $(CXXFLAGS) -I.. -S $< -o $@ -fno-stack-protector

out/test_%.bc: test_%.cpp out
	@echo "Generating bitcode: $@"
	$(CXX) $(CXXFLAGS) -I.. -c $< -o $@ -fno-stack-protector

# Generate IR for runtime tests linked with different backends
out/runtime_simple_tl2.ll: test_runtime_simple.cpp out
	clang -O1 -emit-llvm -S test_runtime_simple.cpp $(TL2_INCLUDE) -o $@ -fno-stack-protector

out/runtime_simple_tinystm.ll: test_runtime_simple.cpp out
	clang -O1 -emit-llvm -S test_runtime_simple.cpp $(TINYSTM_INCLUDE) -o $@ -fno-stack-protector

out/runtime_simple_swiss.ll: test_runtime_simple.cpp out
	clang -O1 -emit-llvm -S test_runtime_simple.cpp $(SWISS_INCLUDE) -o $@ -fno-stack-protector



test_NOrec_counter: test_NOrec_counter.cpp out bin
	@echo "Building $@..."
	$(CXX) $(CXXFLAGS) $(NOREC_INCLUDE) -o bin/$@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

# TinySTM WB-ETL (Write-Back Encounter-Time Locking)
test_runtime_TinySTM_wbetl_counter: test_runtime_TinySTM_counter.cpp out bin
	@echo "Building runtime_tinystm_wbetl with DESIGN_WBCTL..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBETL -o bin/runtime_tinystm_wbetl test_runtime_TinySTM_counter.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

# TinySTM WT (Write-Through)
test_runtime_TinySTM_tinystm_wt_counter: test_runtime_TinySTM_counter.cpp out bin
	@echo "Building runtime_tinystm_wt with DESIGN_WBCTL..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WT -o bin/runtime_tinystm_wt test_runtime_TinySTM_counter.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

# TinySTM WB-CTL (Write-Back Control)
test_runtime_TinySTM_tinystm_wbctl_counter: test_runtime_TinySTM_counter.cpp out bin
	@echo "Building runtime_tinystm_wbctl with DESIGN_WBCTL..."
	$(CXX) $(CXXFLAGS) $(TINYSTM_INCLUDE) -DDESIGN_WBCTL -o bin/runtime_tinystm_wbctl test_runtime_TinySTM_counter.cpp $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)



# Run targets
run: run_simple run_simple2 run_simple_st run_swiss

run_simple: runtime_simple
	@echo "=== Running runtime_simple ==="
	./bin/runtime_simple

run_simple2: runtime_simple2
	@echo "=== Running runtime_simple2 ==="
	./bin/runtime_simple2

run_simple_st: runtime_simple_st
	@echo "=== Running runtime_simple_st ==="
	./bin/runtime_simple_st

run_swiss: runtime_swiss_simple
	@echo "=== Running runtime_swiss_simple ==="
	./bin/runtime_swiss_simple

clean:
	rm -rf $(BIN_DIR) $(OUT_DIR)
	@echo "Cleaned binaries and objects"

help:
	@echo "Available targets:"
	@echo "  all                   - Build all runtime tests"
	@echo "  clean                 - Remove binaries and objects"
	@echo "  run                   - Run all tests"
	@echo "  out/%.ll              - Generate LLVM IR for test"
	@echo "  out/%.bc              - Generate bitcode for test"
	@echo ""
	@echo "Individual build targets:"
	@echo "  runtime_simple        - TinySTM simple test"
	@echo "  runtime_simple2       - TinySTM simple test 2"
	@echo "  runtime_simple_st     - TinySTM single-threaded"
	@echo "  runtime_swiss_simple  - SwissTM simple test"
