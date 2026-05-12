# STAMP Benchmark Makefile

CXX := clang++
CXXFLAGS := -std=c++17 -O1 -pthread
LLVM_PLUGIN_DIR := ../../llvm_tm_plugin
PLUGIN := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so
PLUGIN_PASS := -load-pass-plugin=$(PLUGIN) -passes="tm-instrument"

# TinySTM directories
TINYSTM_DIR := ../../backends/TinySTM
BACKENDS_DIR := ../../backends

OUT_DIR := out
BIN_DIR := bin

HEADERS := $(wildcard *.hpp)

all: stamp_uninstrumented stamp_tinystm

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)

mkdirs:
	mkdir -p $(OUT_DIR) $(BIN_DIR)

# Uninstrumented (native C++ execution)
$(BIN_DIR)/stamp_uninstrumented: STAMP.cpp $(HEADERS) | mkdirs
	$(CXX) $(CXXFLAGS) STAMP.cpp -o $(BIN_DIR)/stamp_uninstrumented

# LLVM IR with -fno-inline to prevent inlining before plugin runs
$(OUT_DIR)/stamp.bc: STAMP.cpp $(HEADERS) | mkdirs
	$(CXX) -std=c++17 -O3 -fno-inline -emit-llvm -c STAMP.cpp -o $@ \
	  -fno-stack-protector -pthread

# Apply plugin instrumentation (method cloning + TX instrumentation)
$(OUT_DIR)/stamp_instr.bc: $(OUT_DIR)/stamp.bc | mkdirs
	opt $(PLUGIN_PASS) $(OUT_DIR)/stamp.bc -o $@

# Optimize instrumented IR: inline cloned methods, optimize TM globals
$(OUT_DIR)/stamp_tinystm.opt.bc: $(OUT_DIR)/stamp_instr.bc | mkdirs
	opt -O3 $(OUT_DIR)/stamp_instr.bc -o $@

# Link with TinySTM runtime
$(BIN_DIR)/stamp_tinystm: $(OUT_DIR)/stamp_tinystm.opt.bc ../../backends/runtimes/TinySTM_runtime.cpp | mkdirs
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL $(OUT_DIR)/stamp_tinystm.opt.bc \
	  ../../backends/runtimes/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $(BIN_DIR)/stamp_tinystm

# Convenience targets
stamp_uninstrumented: $(BIN_DIR)/stamp_uninstrumented
stamp_tinystm: $(BIN_DIR)/stamp_tinystm

run: stamp_uninstrumented
	@echo "Running STAMP (bayes)..."
	@$(BIN_DIR)/stamp_uninstrumented -t 2 -d 2000

.PHONY: all clean run
