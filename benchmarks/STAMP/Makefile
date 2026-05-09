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

all: stamp_uninstrumented stamp_singlelock stamp_tl2 stamp_tinystm stamp_swiss

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)

mkdirs:
	mkdir -p $(OUT_DIR) $(BIN_DIR)

# Uninstrumented (native C++ execution)
$(BIN_DIR)/stamp_uninstrumented: STAMP.cpp | mkdirs
	$(CXX) $(CXXFLAGS) STAMP.cpp -o $(BIN_DIR)/stamp_uninstrumented

# With TL2 runtime
$(OUT_DIR)/stamp.bc: STAMP.cpp | mkdirs
	clang -c -emit-llvm STAMP.cpp -o $(OUT_DIR)/stamp.bc

$(OUT_DIR)/stamp_singlelock.opt.bc: $(OUT_DIR)/stamp.bc | mkdirs
	opt $(PLUGIN_PASS) $(OUT_DIR)/stamp.bc -o $@

$(BIN_DIR)/stamp_singlelock: $(OUT_DIR)/stamp_singlelock.opt.bc ../../backends/runtimes/SingleGlobalLock_runtime.cpp | mkdirs
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/stamp_singlelock.opt.bc ../../backends/runtimes/SingleGlobalLock_runtime.cpp -o $(BIN_DIR)/stamp_singlelock

$(OUT_DIR)/stamp_tl2.opt.bc: $(OUT_DIR)/stamp.bc | mkdirs
	opt $(PLUGIN_PASS) $(OUT_DIR)/stamp.bc -o $(OUT_DIR)/stamp_tl2.opt.bc

$(BIN_DIR)/stamp_tl2: $(OUT_DIR)/stamp_tl2.opt.bc ../../backends/runtimes/TL2_runtime.cpp | mkdirs
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/stamp_tl2.opt.bc ../../backends/runtimes/TL2_runtime.cpp -o $(BIN_DIR)/stamp_tl2

# With TinySTM runtime
$(OUT_DIR)/stamp_tinystm.opt.bc: $(OUT_DIR)/stamp.bc | mkdirs
	opt $(PLUGIN_PASS) $(OUT_DIR)/stamp.bc -o $(OUT_DIR)/stamp_tinystm.opt.bc

$(BIN_DIR)/stamp_tinystm: $(OUT_DIR)/stamp_tinystm.opt.bc ../../backends/runtimes/TinySTM_runtime.cpp | mkdirs
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL $(OUT_DIR)/stamp_tinystm.opt.bc \
	  ../../backends/runtimes/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $(BIN_DIR)/stamp_tinystm

# With SwissTM runtime
$(OUT_DIR)/stamp_swiss.opt.bc: $(OUT_DIR)/stamp.bc | mkdirs
	opt $(PLUGIN_PASS) $(OUT_DIR)/stamp.bc -o $(OUT_DIR)/stamp_swiss.opt.bc

$(BIN_DIR)/stamp_swiss: $(OUT_DIR)/stamp_swiss.opt.bc ../../backends/runtimes/SwissTM_runtime.cpp | mkdirs
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/stamp_swiss.opt.bc ../../backends/runtimes/SwissTM_runtime.cpp -o $(BIN_DIR)/stamp_swiss

# Convenience targets
stamp_uninstrumented: $(BIN_DIR)/stamp_uninstrumented
stamp_singlelock: $(BIN_DIR)/stamp_singlelock
stamp_tl2: $(BIN_DIR)/stamp_tl2
stamp_tinystm: $(BIN_DIR)/stamp_tinystm
stamp_swiss: $(BIN_DIR)/stamp_swiss

run: stamp_uninstrumented
	@echo "Running STAMP (bayes as default)..."
	@$(BIN_DIR)/stamp_uninstrumented -t 2 -d 2000

.PHONY: all clean run